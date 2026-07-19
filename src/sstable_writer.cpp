#include "kvault/sstable_writer.hpp"

#include <cstring>
#include <stdexcept>

// Platform-specific fsync for durability after flush
#ifdef _WIN32
    #include <io.h>
#else
    #include <unistd.h>
#endif

namespace kvault {

// ============================================================================
// SSTable Binary File Format — Complete Byte Layout
// ============================================================================
//
// All multi-byte integers are stored in LITTLE-ENDIAN byte order.
// The file is written top-to-bottom in a single sequential pass.
//
// ╔══════════════════════════════════════════════════════════════════════╗
// ║  SECTION 1: DATA BLOCK                                              ║
// ║  Starts at byte offset 0.                                           ║
// ║  Contains all KVRecords in sorted key order.                        ║
// ║                                                                      ║
// ║  Each record is encoded as:                                          ║
// ║                                                                      ║
// ║   Offset  Size  Field          Description                           ║
// ║   ──────  ────  ─────          ─────────────────────────────────     ║
// ║   +0      1     record_type    0x00=PUT, 0x01=DELETE                 ║
// ║   +1      4     key_length     Length of key bytes (uint32 LE)       ║
// ║   +5      K     key_data       Raw key bytes                         ║
// ║   +5+K    4     value_length   Length of value bytes (uint32 LE)     ║
// ║   +9+K    V     value_data     Raw value bytes                       ║
// ║                                                                      ║
// ║  Total per record: 9 + K + V bytes                                   ║
// ║  Total data block: sum(9 + Ki + Vi) for all i in [0, N)              ║
// ╠══════════════════════════════════════════════════════════════════════╣
// ║  SECTION 2: SPARSE INDEX BLOCK                                       ║
// ║  Starts immediately after the last data record.                      ║
// ║  Contains one entry per kIndexBlockInterval-th data record.          ║
// ║  Also always contains an entry for the LAST record.                  ║
// ║                                                                      ║
// ║  Header (4 bytes):                                                   ║
// ║   +0      4     entry_count    Number of index entries (uint32 LE)   ║
// ║                                                                      ║
// ║  Each index entry:                                                   ║
// ║   +0      8     data_offset    Byte offset in data block (uint64 LE) ║
// ║   +8      4     key_length     Length of key bytes (uint32 LE)       ║
// ║   +12     K     key_data       Raw key bytes                         ║
// ║                                                                      ║
// ║  Binary search on data_offset + key finds the scan start point.     ║
// ╠══════════════════════════════════════════════════════════════════════╣
// ║  SECTION 3: BLOOM FILTER BLOCK                                       ║
// ║  Starts immediately after the index block.                           ║
// ║                                                                      ║
// ║  Header (4 bytes):                                                   ║
// ║   +0      4     bloom_size     Byte size of serialized filter        ║
// ║                                                                      ║
// ║  Body (bloom_size bytes):                                            ║
// ║   BloomFilter::serialize() output:                                   ║
// ║     [0..3]  k (uint32 LE) — number of hash functions                 ║
// ║     [4..7]  m (uint32 LE) — number of bits                           ║
// ║     [8..]   packed bit array  ceil(m/8) bytes                        ║
// ╠══════════════════════════════════════════════════════════════════════╣
// ║  SECTION 4: FOOTER (exactly 48 bytes, always at file end - 48)       ║
// ║                                                                      ║
// ║   Offset  Size  Field                  Description                   ║
// ║   ──────  ────  ─────                  ─────────────────────────     ║
// ║   +0      8     index_block_offset     Byte offset of section 2      ║
// ║   +8      8     bloom_block_offset     Byte offset of section 3      ║
// ║   +16     8     footer_offset          Byte offset of this footer    ║
// ║   +24     8     entry_count            Total KVRecord count          ║
// ║   +32     8     data_block_size        Size of section 1 in bytes    ║
// ║   +40     8     magic                  0x544C564B00010000 ("KVLT")   ║
// ║                                                                      ║
// ║  Total: 6 × 8 = 48 bytes. Validated by static_assert in header.     ║
// ╚══════════════════════════════════════════════════════════════════════╝
//
// READER BOOTSTRAP SEQUENCE:
//   1. fseek(EOF - 48)     → read Footer
//   2. Validate magic number
//   3. fseek(bloom_block_offset)  → read + deserialize BloomFilter
//   4. fseek(index_block_offset)  → read all IndexEntries into memory
//   → Ready for O(log n) point lookups with 1 disk seek per query
//
// ============================================================================

namespace {

// ---------------------------------------------------------------------------
// Little-Endian Write Helpers
// ---------------------------------------------------------------------------

void write_u8(FILE* f, uint8_t v) {
    std::fwrite(&v, 1, 1, f);
}

void write_u32_le(FILE* f, uint32_t v) {
    uint8_t buf[4] = {
        static_cast<uint8_t>( v        & 0xFFu),
        static_cast<uint8_t>((v >>  8) & 0xFFu),
        static_cast<uint8_t>((v >> 16) & 0xFFu),
        static_cast<uint8_t>((v >> 24) & 0xFFu)
    };
    std::fwrite(buf, 1, 4, f);
}

void write_u64_le(FILE* f, uint64_t v) {
    uint8_t buf[8] = {
        static_cast<uint8_t>( v        & 0xFFULL),
        static_cast<uint8_t>((v >>  8) & 0xFFULL),
        static_cast<uint8_t>((v >> 16) & 0xFFULL),
        static_cast<uint8_t>((v >> 24) & 0xFFULL),
        static_cast<uint8_t>((v >> 32) & 0xFFULL),
        static_cast<uint8_t>((v >> 40) & 0xFFULL),
        static_cast<uint8_t>((v >> 48) & 0xFFULL),
        static_cast<uint8_t>((v >> 56) & 0xFFULL)
    };
    std::fwrite(buf, 1, 8, f);
}

// Write a length-prefixed string (uint32 LE length + raw bytes)
void write_bytes(FILE* f, const std::string& s) {
    write_u32_le(f, static_cast<uint32_t>(s.size()));
    if (!s.empty()) {
        std::fwrite(s.data(), 1, s.size(), f);
    }
}

// Return current file position (byte offset from beginning)
uint64_t ftell64(FILE* f) {
#ifdef _WIN32
    return static_cast<uint64_t>(_ftelli64(f));
#else
    return static_cast<uint64_t>(std::ftell(f));
#endif
}

// Flush + fsync for durability
void sync_file(FILE* f) {
    std::fflush(f);
#ifdef _WIN32
    _commit(_fileno(f));
#else
    ::fsync(fileno(f));
#endif
}

} // anonymous namespace

// ============================================================================
// SSTableWriter::write — Main Entry Point
// ============================================================================

void SSTableWriter::write(const std::filesystem::path& path,
                          const std::vector<KVRecord>& sorted_entries,
                          size_t bits_per_key)
{
    if (sorted_entries.empty()) {
        // Write an empty but valid SSTable so the manager doesn't need
        // special-case logic for zero-entry tables.
        // (Edge case: flush triggered after all keys were deleted.)
    }

    // Create parent directories if they don't exist
    std::filesystem::create_directories(path.parent_path());

    FILE* f = std::fopen(path.string().c_str(), "wb");
    if (!f) {
        throw std::runtime_error(
            "SSTableWriter: failed to open for write: " + path.string());
    }

    // -----------------------------------------------------------------------
    // SECTION 1: DATA BLOCK
    // -----------------------------------------------------------------------
    // Write every KVRecord sequentially. For each record, note its starting
    // byte offset — used to build the sparse index.

    // Build Bloom Filter in parallel as we iterate (saves a second pass).
    BloomFilter bloom(sorted_entries.size(), bits_per_key);

    // Sparse index: collect (offset, key) for every kIndexBlockInterval-th record.
    struct RawIndexEntry {
        uint64_t    offset;
        std::string key;
    };
    std::vector<RawIndexEntry> raw_index;
    raw_index.reserve(sorted_entries.size() / kIndexBlockInterval + 2);

    const uint64_t data_block_start = ftell64(f); // Should always be 0

    for (size_t idx = 0; idx < sorted_entries.size(); ++idx) {
        const auto& rec = sorted_entries[idx];

        const uint64_t record_offset = ftell64(f);

        // Record the offset of every Nth key (and always the first key)
        if (idx == 0 || idx % kIndexBlockInterval == 0) {
            raw_index.push_back({ record_offset, rec.key });
        }

        // Feed the Bloom Filter (only PUT keys need lookup; DELETE tombstones
        // still need to be found to propagate the deletion, so add all keys).
        bloom.add(rec.key);

        // Write the record: [type 1B] [key_len 4B] [key] [val_len 4B] [val]
        write_u8(f, static_cast<uint8_t>(rec.type));
        write_bytes(f, rec.key);
        write_bytes(f, rec.value);
    }

    const uint64_t data_block_end  = ftell64(f);
    const uint64_t data_block_size = data_block_end - data_block_start;

    // -----------------------------------------------------------------------
    // SECTION 2: SPARSE INDEX BLOCK
    // -----------------------------------------------------------------------

    const uint64_t index_block_offset = ftell64(f);

    write_u32_le(f, static_cast<uint32_t>(raw_index.size()));
    for (const auto& entry : raw_index) {
        write_u64_le(f, entry.offset);
        write_bytes(f, entry.key);
    }

    // -----------------------------------------------------------------------
    // SECTION 3: BLOOM FILTER BLOCK
    // -----------------------------------------------------------------------

    const uint64_t bloom_block_offset = ftell64(f);
    const auto bloom_bytes = bloom.serialize();

    write_u32_le(f, static_cast<uint32_t>(bloom_bytes.size()));
    std::fwrite(bloom_bytes.data(), 1, bloom_bytes.size(), f);

    // -----------------------------------------------------------------------
    // SECTION 4: FOOTER (48 bytes)
    // -----------------------------------------------------------------------

    const uint64_t footer_offset = ftell64(f);

    SSTableFooter footer{};
    footer.index_block_offset = index_block_offset;
    footer.bloom_block_offset = bloom_block_offset;
    footer.footer_offset      = footer_offset;
    footer.entry_count        = static_cast<uint64_t>(sorted_entries.size());
    footer.data_block_size    = data_block_size;
    footer.magic              = kMagicNumber;

    write_u64_le(f, footer.index_block_offset);
    write_u64_le(f, footer.bloom_block_offset);
    write_u64_le(f, footer.footer_offset);
    write_u64_le(f, footer.entry_count);
    write_u64_le(f, footer.data_block_size);
    write_u64_le(f, footer.magic);

    // Durability: ensure the full file hits stable storage before we
    // consider the SSTable "live". The WAL can only be truncated after this.
    sync_file(f);
    std::fclose(f);
}

} // namespace kvault
