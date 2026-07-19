#include "kvault/sstable_reader.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace kvault {

// ============================================================================
// Little-Endian Read Helpers (file position advances after each call)
// ============================================================================

namespace {

bool read_u8(FILE* f, uint8_t& out) {
    return std::fread(&out, 1, 1, f) == 1;
}

bool read_u32_le(FILE* f, uint32_t& out) {
    uint8_t buf[4];
    if (std::fread(buf, 1, 4, f) != 4) return false;
    out =  static_cast<uint32_t>(buf[0])
        | (static_cast<uint32_t>(buf[1]) <<  8)
        | (static_cast<uint32_t>(buf[2]) << 16)
        | (static_cast<uint32_t>(buf[3]) << 24);
    return true;
}

bool read_u64_le(FILE* f, uint64_t& out) {
    uint8_t buf[8];
    if (std::fread(buf, 1, 8, f) != 8) return false;
    out =  static_cast<uint64_t>(buf[0])
        | (static_cast<uint64_t>(buf[1]) <<  8)
        | (static_cast<uint64_t>(buf[2]) << 16)
        | (static_cast<uint64_t>(buf[3]) << 24)
        | (static_cast<uint64_t>(buf[4]) << 32)
        | (static_cast<uint64_t>(buf[5]) << 40)
        | (static_cast<uint64_t>(buf[6]) << 48)
        | (static_cast<uint64_t>(buf[7]) << 56);
    return true;
}

// Read a length-prefixed string written by write_bytes() in the writer.
bool read_string(FILE* f, std::string& out) {
    uint32_t len = 0;
    if (!read_u32_le(f, len)) return false;
    out.resize(len);
    if (len > 0 && std::fread(out.data(), 1, len, f) != len) return false;
    return true;
}

// Platform-portable 64-bit fseek
int fseek64(FILE* f, int64_t offset, int origin) {
#ifdef _WIN32
    return _fseeki64(f, offset, origin);
#else
    return std::fseek(f, static_cast<long>(offset), origin);
#endif
}

int64_t ftell64(FILE* f) {
#ifdef _WIN32
    return _ftelli64(f);
#else
    return static_cast<int64_t>(std::ftell(f));
#endif
}

} // anonymous namespace

// ============================================================================
// Construction — Open File and Load Metadata
// ============================================================================

SSTableReader::SSTableReader(const std::filesystem::path& path)
    : path_(path)
    , file_(nullptr)
    , bloom_filter_(1, 10) // temporary; overwritten by load_bloom_filter()
    , entry_count_(0)
    , data_block_size_(0)
    , index_block_offset_(0)
{
    file_ = std::fopen(path_.string().c_str(), "rb");
    if (!file_) {
        throw std::runtime_error(
            "SSTableReader: cannot open file: " + path_.string());
    }

    // Bootstrap sequence (see READER BOOTSTRAP SEQUENCE in sstable_writer.cpp):
    //   1. Read footer (from end of file)
    //   2. Load bloom filter (from bloom_block_offset)
    //   3. Load index (from index_block_offset)
    const SSTableFooter footer = read_footer();

    if (footer.magic != SSTableWriter::kMagicNumber) {
        std::fclose(file_);
        file_ = nullptr;
        throw std::runtime_error(
            "SSTableReader: invalid magic number in " + path_.string());
    }

    entry_count_        = footer.entry_count;
    data_block_size_    = footer.data_block_size;
    index_block_offset_ = footer.index_block_offset;

    load_bloom_filter(footer.bloom_block_offset, footer.footer_offset);
    load_index(footer.index_block_offset, footer.bloom_block_offset);
}

SSTableReader::~SSTableReader() {
    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }
}

// Move semantics
SSTableReader::SSTableReader(SSTableReader&& other) noexcept
    : path_(std::move(other.path_))
    , file_(other.file_)
    , index_(std::move(other.index_))
    , bloom_filter_(std::move(other.bloom_filter_))
    , entry_count_(other.entry_count_)
    , data_block_size_(other.data_block_size_)
    , min_key_(std::move(other.min_key_))
    , max_key_(std::move(other.max_key_))
{
    other.file_ = nullptr;
}

SSTableReader& SSTableReader::operator=(SSTableReader&& other) noexcept {
    if (this != &other) {
        if (file_) std::fclose(file_);
        path_            = std::move(other.path_);
        file_            = other.file_;
        index_           = std::move(other.index_);
        bloom_filter_    = std::move(other.bloom_filter_);
        entry_count_     = other.entry_count_;
        data_block_size_ = other.data_block_size_;
        min_key_         = std::move(other.min_key_);
        max_key_         = std::move(other.max_key_);
        other.file_      = nullptr;
    }
    return *this;
}

// ============================================================================
// read_footer — Read the Fixed 48-byte Footer from File End
// ============================================================================

SSTableFooter SSTableReader::read_footer() const {
    // Seek to file end - 48 bytes
    fseek64(file_, -static_cast<int64_t>(SSTableFooter::kSerializedSize), SEEK_END);

    SSTableFooter footer{};
    if (!read_u64_le(file_, footer.index_block_offset) ||
        !read_u64_le(file_, footer.bloom_block_offset)  ||
        !read_u64_le(file_, footer.footer_offset)        ||
        !read_u64_le(file_, footer.entry_count)          ||
        !read_u64_le(file_, footer.data_block_size)      ||
        !read_u64_le(file_, footer.magic))
    {
        throw std::runtime_error(
            "SSTableReader: failed to read footer from " + path_.string());
    }
    return footer;
}

// ============================================================================
// load_bloom_filter — Deserialize Filter from Bloom Block
// ============================================================================

void SSTableReader::load_bloom_filter(uint64_t bloom_block_offset,
                                       uint64_t footer_offset)
{
    fseek64(file_, static_cast<int64_t>(bloom_block_offset), SEEK_SET);

    // Read the 4-byte size prefix
    uint32_t bloom_size = 0;
    if (!read_u32_le(file_, bloom_size)) {
        throw std::runtime_error(
            "SSTableReader: failed to read bloom filter size");
    }

    // Sanity check: bloom block must fit between its offset and the footer
    const uint64_t max_bloom_size =
        footer_offset - bloom_block_offset - 4;
    if (static_cast<uint64_t>(bloom_size) > max_bloom_size) {
        throw std::runtime_error(
            "SSTableReader: bloom filter size exceeds available space");
    }

    // Read the serialized filter bytes
    std::vector<uint8_t> bloom_data(bloom_size);
    if (bloom_size > 0 &&
        std::fread(bloom_data.data(), 1, bloom_size, file_) != bloom_size) {
        throw std::runtime_error(
            "SSTableReader: truncated bloom filter block");
    }

    bloom_filter_ = BloomFilter::deserialize(bloom_data);
}

// ============================================================================
// load_index — Build In-Memory Sparse Index from Index Block
// ============================================================================

void SSTableReader::load_index(uint64_t index_block_offset,
                                uint64_t bloom_block_offset)
{
    fseek64(file_, static_cast<int64_t>(index_block_offset), SEEK_SET);

    uint32_t entry_count = 0;
    if (!read_u32_le(file_, entry_count)) {
        throw std::runtime_error(
            "SSTableReader: failed to read index entry count");
    }

    index_.reserve(entry_count);

    for (uint32_t i = 0; i < entry_count; ++i) {
        IndexEntry entry;
        uint64_t   offset = 0;
        if (!read_u64_le(file_, offset) ||
            !read_string(file_, entry.key))
        {
            throw std::runtime_error(
                "SSTableReader: corrupted index block");
        }
        entry.offset = offset;
        index_.push_back(std::move(entry));
    }

    // Populate min/max key from the index (first and last entries)
    if (!index_.empty()) {
        min_key_ = index_.front().key;
        // max_key_ is the last index entry's key (≤ actual last key).
        // For an exact max_key, we'd need to scan; the index key is safe
        // for range-filter decisions (it's a lower bound of the last block).
        max_key_ = index_.back().key;
    }

    (void)bloom_block_offset; // Used only for bounds checking in load_bloom
}

// ============================================================================
// read_record — Deserialize One KVRecord from Current File Position
// ============================================================================

bool SSTableReader::read_record(FILE* file, KVRecord& out) {
    uint8_t type_byte = 0;
    if (!read_u8(file, type_byte))          return false;
    if (!read_string(file, out.key))        return false;
    if (!read_string(file, out.value))      return false;

    out.type = static_cast<RecordType>(type_byte);
    return true;
}

// ============================================================================
// get() — Three-Step Lookup
// ============================================================================
//
// Step 1: Bloom Filter  — O(k) in-memory bit checks
// Step 2: Binary Search — O(log(N/interval)) in-memory index scan
// Step 3: Data Scan     — O(interval) sequential disk reads (1 seek)
//
// ============================================================================

std::optional<Value> SSTableReader::get(const Key& key) const {
    auto record = get_record(key);
    if (!record) return std::nullopt;

    // Surface tombstones as empty string — the KVStore distinguishes this
    // from "not found" using contains_tombstone() on the MemTable first.
    // If a tombstone reaches here (from a flushed MemTable), return "".
    if (record->type == RecordType::DELETE) {
        return std::string{}; // Tombstone marker (empty value)
    }
    return record->value;
}

std::optional<KVRecord> SSTableReader::get_record(const Key& key) const {
    // ── Step 1: Bloom Filter Check ─────────────────────────────────────────
    if (!bloom_filter_.might_contain(key)) {
        return std::nullopt;  // Definite miss — zero disk I/O
    }

    // ── Step 2: Sparse Index Binary Search ─────────────────────────────────
    // Find the last index entry whose key ≤ target key.
    // That entry's offset is where we start scanning the data block.
    //
    // Using upper_bound then stepping back:
    //   upper_bound finds the first entry with key > target.
    //   The entry before it is the last with key ≤ target.

    if (index_.empty()) return std::nullopt;

    auto it = std::upper_bound(
        index_.begin(), index_.end(), key,
        [](const Key& k, const IndexEntry& e) { return k < e.key; }
    );

    // If all index keys are > target, the key can't be in this SSTable
    // (since the data is sorted, and the first index key is the minimum).
    if (it == index_.begin()) {
        return std::nullopt;
    }

    // Step back to get the last entry with key ≤ target
    --it;
    const uint64_t scan_start = it->offset;

    // Determine the scan end: use the NEXT index entry's offset as the
    // upper bound. If this is the last index entry, scan until data block end.
    uint64_t scan_end;
    const auto next_it = std::next(it);
    if (next_it != index_.end()) {
        scan_end = next_it->offset;
    } else {
        // Last sparse block: scan until the start of the index block,
        // which is the exact byte boundary between data and metadata.
        scan_end = index_block_offset_;
    }

    // ── Step 3: Data Block Scan ─────────────────────────────────────────────
    // Seek once, then read records sequentially until we find the key or
    // exhaust the sparse block's byte range.

    fseek64(file_, static_cast<int64_t>(scan_start), SEEK_SET);

    while (static_cast<uint64_t>(ftell64(file_)) < scan_end) {
        KVRecord record;
        if (!read_record(file_, record)) {
            break; // EOF or read error — key not found in this range
        }

        if (record.key == key) {
            return record;  // Found!
        }

        // Since data is sorted, if we've passed the target key we can stop.
        if (record.key > key) {
            break;
        }
    }

    return std::nullopt; // Not in this SSTable
}

// ============================================================================
// Observers
// ============================================================================

const std::string& SSTableReader::min_key() const noexcept {
    return min_key_;
}

const std::string& SSTableReader::max_key() const noexcept {
    return max_key_;
}

uint64_t SSTableReader::entry_count() const noexcept {
    return entry_count_;
}

uint64_t SSTableReader::file_size() const {
    if (!std::filesystem::exists(path_)) return 0;
    return static_cast<uint64_t>(std::filesystem::file_size(path_));
}

const std::filesystem::path& SSTableReader::path() const noexcept {
    return path_;
}

SSTableMetadata SSTableReader::metadata() const {
    return SSTableMetadata{
        .min_key         = min_key_,
        .max_key         = max_key_,
        .entry_count     = entry_count_,
        .file_size_bytes = file_size(),
        .file_path       = path_.string()
    };
}

} // namespace kvault
