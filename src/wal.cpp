#include "kvault/wal.hpp"

#include <array>
#include <cstring>
#include <stdexcept>

// Platform-specific headers for hardware sync (fsync / _commit)
#ifdef _WIN32
    #include <io.h>       // _fileno, _commit
#else
    #include <unistd.h>   // fileno, fsync
#endif

namespace kvault {

// ============================================================================
// CRC32 — Compile-Time Lookup Table
// ============================================================================
//
// Standard CRC32 using the reflected polynomial 0xEDB88320 (same as
// Ethernet, zlib, and PNG). Used to detect corrupted WAL records after
// a crash — e.g., a partial write where only half the record made it
// to disk before power was lost.
//
// The 256-entry lookup table is generated at compile time via constexpr,
// so there's zero runtime initialization cost.
// ============================================================================

namespace {

constexpr std::array<uint32_t, 256> generate_crc32_table() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (int j = 0; j < 8; ++j) {
            crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
        }
        table[i] = crc;
    }
    return table;
}

// Computed at compile time — stored in the binary's read-only data segment.
constexpr auto kCRC32Table = generate_crc32_table();

// ---------------------------------------------------------------------------
// Little-Endian Serialization Helpers
// ---------------------------------------------------------------------------
// These functions explicitly encode/decode integers byte-by-byte in
// little-endian order, making the WAL format portable across architectures
// (even though x86/x64 and modern ARM are natively little-endian).
// ---------------------------------------------------------------------------

void write_u32_le(std::vector<uint8_t>& buf, uint32_t val) {
    buf.push_back(static_cast<uint8_t>( val        & 0xFFu));
    buf.push_back(static_cast<uint8_t>((val >>  8) & 0xFFu));
    buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFFu));
    buf.push_back(static_cast<uint8_t>((val >> 24) & 0xFFu));
}

uint32_t read_u32_le(const uint8_t* data) {
    return  static_cast<uint32_t>(data[0])
         | (static_cast<uint32_t>(data[1]) <<  8)
         | (static_cast<uint32_t>(data[2]) << 16)
         | (static_cast<uint32_t>(data[3]) << 24);
}

// Read exactly `count` bytes from `file` into `buf`.
// Returns false on EOF or short read (fewer bytes available than requested).
bool read_exact(FILE* file, void* buf, size_t count) {
    return std::fread(buf, 1, count, file) == count;
}

} // anonymous namespace

// ============================================================================
// CRC32 Computation
// ============================================================================

uint32_t WriteAheadLog::compute_crc32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) {
        crc = kCRC32Table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

// ============================================================================
// Construction & Destruction
// ============================================================================

WriteAheadLog::WriteAheadLog(const std::filesystem::path& wal_directory,
                             bool sync_per_write)
    : path_(wal_directory / kWALFilename)
    , sync_per_write_(sync_per_write)
    , write_handle_(nullptr)
{
    // Ensure the WAL directory exists (creates parent dirs if needed).
    std::filesystem::create_directories(wal_directory);
    open_for_append();
}

WriteAheadLog::~WriteAheadLog() {
    close_handle();
}

void WriteAheadLog::open_for_append() {
    // "ab" = append + binary. Creates the file if it doesn't exist.
    // Existing content is preserved; new writes go to the end.
    write_handle_ = std::fopen(path_.string().c_str(), "ab");
    if (!write_handle_) {
        throw std::runtime_error(
            "WAL: failed to open file for append: " + path_.string());
    }
}

void WriteAheadLog::close_handle() {
    if (write_handle_) {
        std::fclose(write_handle_);
        write_handle_ = nullptr;
    }
}

// ============================================================================
// Platform-Specific Sync
// ============================================================================
//
// Two levels of durability:
//
//   fflush()  — pushes data from the C library's userspace buffer into
//               the OS kernel's page cache. Fast, but data can be lost
//               if the machine loses power (page cache is in RAM).
//
//   fsync()   — forces the OS to write the page cache to the physical
//               storage device. Slower (involves disk I/O), but the
//               data survives power failures.
//
// When sync_per_write_ is false, we only fflush(). When true, we also
// fsync() via the platform-appropriate system call.
// ============================================================================

void WriteAheadLog::sync_to_disk() {
    if (!write_handle_) return;

    // Step 1: Always flush the userspace buffer to the OS.
    std::fflush(write_handle_);

    // Step 2: Optionally force the OS to write to stable storage.
    if (sync_per_write_) {
#ifdef _WIN32
        const int fd = _fileno(write_handle_);
        _commit(fd);
#else
        const int fd = fileno(write_handle_);
        ::fsync(fd);
#endif
    }
}

// ============================================================================
// Binary Serialization
// ============================================================================
//
// WAL Record Layout (all multi-byte fields are little-endian):
//
// Offset   Size     Field           Description
// ──────   ──────   ─────────────   ──────────────────────────────────────
// 0        1        record_type     0x00 = PUT, 0x01 = DELETE
// 1        4        key_length      Length of key_data in bytes (uint32 LE)
// 5        K        key_data        Raw key bytes (K = key_length)
// 5+K      4        value_length    Length of value_data in bytes (uint32 LE)
// 9+K      V        value_data      Raw value bytes (V = value_length)
// 9+K+V    4        crc32           CRC32 of bytes [0 .. 9+K+V) (uint32 LE)
// ──────   ──────   ─────────────   ──────────────────────────────────────
// Total:   13 + K + V bytes per record
//
// The CRC32 covers the ENTIRE record payload (type + key_len + key +
// val_len + val) but NOT itself. During replay, we recompute the CRC32
// over the payload and compare it against the stored value. A mismatch
// indicates corruption (e.g., partial write from a crash).
//
// Example: PUT("hello", "world")
//   type       = 0x00
//   key_len    = 0x05 0x00 0x00 0x00  (5 in LE)
//   key_data   = 0x68 0x65 0x6C 0x6C 0x6F  ("hello")
//   val_len    = 0x05 0x00 0x00 0x00  (5 in LE)
//   val_data   = 0x77 0x6F 0x72 0x6C 0x64  ("world")
//   crc32      = [4 bytes computed over the 23 payload bytes above]
//   Total: 27 bytes
//
// ============================================================================

std::vector<uint8_t> WriteAheadLog::serialize_record(const KVRecord& record) {
    const auto key_len = static_cast<uint32_t>(record.key.size());
    const auto val_len = static_cast<uint32_t>(record.value.size());

    // Pre-allocate the exact buffer size:
    //   1 (type) + 4 (key_len) + K + 4 (val_len) + V + 4 (crc) = 13 + K + V
    std::vector<uint8_t> buf;
    buf.reserve(static_cast<size_t>(13) + key_len + val_len);

    // 1. Record type (1 byte)
    buf.push_back(static_cast<uint8_t>(record.type));

    // 2. Key length (4 bytes LE) + key data
    write_u32_le(buf, key_len);
    buf.insert(buf.end(), record.key.begin(), record.key.end());

    // 3. Value length (4 bytes LE) + value data
    write_u32_le(buf, val_len);
    buf.insert(buf.end(), record.value.begin(), record.value.end());

    // 4. CRC32 over all preceding bytes (the payload)
    const uint32_t crc = compute_crc32(buf.data(), buf.size());
    write_u32_le(buf, crc);

    return buf;
}

// ============================================================================
// Binary Deserialization (used by replay)
// ============================================================================
//
// Reads one record from the current file position. Returns std::nullopt
// if the record is incomplete (truncated by crash) or has a bad CRC32.
//
// After a failed deserialization, the file position is indeterminate —
// the caller should stop reading (we can't determine the next record
// boundary after corruption).
// ============================================================================

std::optional<KVRecord> WriteAheadLog::deserialize_record(FILE* file) {
    // Accumulate the payload bytes for CRC verification.
    std::vector<uint8_t> payload;

    // 1. Record type (1 byte)
    uint8_t type_byte = 0;
    if (!read_exact(file, &type_byte, 1)) {
        return std::nullopt;  // EOF — no more records
    }
    if (type_byte > static_cast<uint8_t>(RecordType::DELETE)) {
        return std::nullopt;  // Invalid type — corruption
    }
    payload.push_back(type_byte);

    // 2. Key length (4 bytes LE)
    uint8_t len_buf[4];
    if (!read_exact(file, len_buf, 4)) {
        return std::nullopt;  // Truncated record
    }
    payload.insert(payload.end(), len_buf, len_buf + 4);
    const uint32_t key_len = read_u32_le(len_buf);

    // Sanity bound: reject absurdly large keys (> 16 MB) to prevent
    // OOM from corrupted length fields.
    if (key_len > 16u * 1024 * 1024) {
        return std::nullopt;
    }

    // 3. Key data
    std::string key(key_len, '\0');
    if (key_len > 0 && !read_exact(file, key.data(), key_len)) {
        return std::nullopt;  // Truncated record
    }
    payload.insert(payload.end(), key.begin(), key.end());

    // 4. Value length (4 bytes LE)
    if (!read_exact(file, len_buf, 4)) {
        return std::nullopt;  // Truncated record
    }
    payload.insert(payload.end(), len_buf, len_buf + 4);
    const uint32_t val_len = read_u32_le(len_buf);

    if (val_len > 16u * 1024 * 1024) {
        return std::nullopt;
    }

    // 5. Value data
    std::string value(val_len, '\0');
    if (val_len > 0 && !read_exact(file, value.data(), val_len)) {
        return std::nullopt;  // Truncated record
    }
    payload.insert(payload.end(), value.begin(), value.end());

    // 6. Stored CRC32 (4 bytes LE)
    uint8_t crc_buf[4];
    if (!read_exact(file, crc_buf, 4)) {
        return std::nullopt;  // Truncated record
    }
    const uint32_t stored_crc   = read_u32_le(crc_buf);
    const uint32_t computed_crc = compute_crc32(payload.data(), payload.size());

    // 7. CRC verification
    if (stored_crc != computed_crc) {
        // Checksum mismatch — this record (and all following) is corrupted.
        // This typically happens when a crash interrupted a write mid-record.
        return std::nullopt;
    }

    // Build the validated KVRecord.
    KVRecord record;
    record.type  = static_cast<RecordType>(type_byte);
    record.key   = std::move(key);
    record.value = std::move(value);
    return record;
}

// ============================================================================
// append() — Write a Record to the WAL
// ============================================================================

void WriteAheadLog::append(const KVRecord& record) {
    if (!write_handle_) {
        throw std::runtime_error("WAL: cannot append — file handle is closed");
    }

    const auto serialized = serialize_record(record);

    const size_t written = std::fwrite(
        serialized.data(), 1, serialized.size(), write_handle_);

    if (written != serialized.size()) {
        throw std::runtime_error(
            "WAL: short write — expected " +
            std::to_string(serialized.size()) +
            " bytes, wrote " + std::to_string(written));
    }

    sync_to_disk();
}

// ============================================================================
// replay() — Recovery: Read All Valid Records
// ============================================================================
//
// Opens the WAL file in read-only mode and deserializes records one by
// one until:
//   - EOF is reached (all records are valid), or
//   - A record fails deserialization (CRC mismatch or truncation).
//
// Records after the first failure are discarded. This is safe because
// the WAL is append-only — a corrupted record means a crash occurred
// during that write, so no subsequent records could have been fully
// written either.
// ============================================================================

std::vector<KVRecord> WriteAheadLog::replay() const {
    std::vector<KVRecord> records;

    // Open a SEPARATE read handle (the write handle stays open for appends).
    FILE* read_handle = std::fopen(path_.string().c_str(), "rb");
    if (!read_handle) {
        // WAL file doesn't exist yet — first run, nothing to replay.
        return records;
    }

    while (true) {
        auto record = deserialize_record(read_handle);
        if (!record.has_value()) {
            break;  // EOF or corruption — stop here
        }
        records.push_back(std::move(*record));
    }

    std::fclose(read_handle);
    return records;
}

// ============================================================================
// truncate() — Clear the WAL After Successful Flush
// ============================================================================
//
// After the MemTable has been successfully flushed to an SSTable, all
// records in the WAL are redundant (they exist on disk in the SSTable).
// Truncating the WAL reclaims disk space and ensures that recovery
// doesn't replay already-persisted records.
// ============================================================================

void WriteAheadLog::truncate() {
    close_handle();

    // Truncate the file to zero bytes using the C++17 filesystem API.
    std::filesystem::resize_file(path_, 0);

    // Reopen in append mode for future writes.
    open_for_append();
}

// ============================================================================
// Observers
// ============================================================================

size_t WriteAheadLog::file_size() const {
    if (!std::filesystem::exists(path_)) {
        return 0;
    }
    return static_cast<size_t>(std::filesystem::file_size(path_));
}

const std::filesystem::path& WriteAheadLog::file_path() const noexcept {
    return path_;
}

} // namespace kvault
