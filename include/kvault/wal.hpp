#pragma once

// ============================================================================
// wal.hpp — Write-Ahead Log for Crash Recovery
// ============================================================================
//
// The WAL is an append-only binary log that records every mutation BEFORE
// it is applied to the MemTable. This guarantees that no acknowledged
// write is ever lost, even if the process crashes immediately after.
//
// Durability Contract:
//   1. KVStore receives a PUT/DELETE request
//   2. WAL::append() serializes the record and writes it to disk
//   3. If sync_per_write is true, fsync() ensures stable storage
//   4. Only THEN does the MemTable get updated
//   5. The response is sent to the client
//
// Recovery Protocol (on startup):
//   1. WAL::replay() reads all valid records from the log file
//   2. Records are re-applied to a fresh MemTable
//   3. If a record has a bad CRC32, it and all subsequent records are
//      discarded (they represent a partial write from a crash)
//
// Lifecycle:
//   The WAL is truncated (cleared) after the MemTable has been
//   successfully flushed to an SSTable. At that point, all records
//   in the WAL are redundant — they exist on disk in the SSTable.
//
// Binary Format:
//   See the detailed layout diagram in wal.cpp, serialize_record().
//
// Thread Safety: NOT thread-safe. The KVStore layer serializes access.
// ============================================================================

#include "kvault/types.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace kvault {

class WriteAheadLog {
public:
    // -----------------------------------------------------------------------
    // Construction & Destruction
    // -----------------------------------------------------------------------

    // Opens (or creates) the WAL file in the specified directory.
    // Creates the directory if it doesn't exist.
    //
    // @param wal_directory   Path to the WAL directory (e.g., "data/wal")
    // @param sync_per_write  If true, every append() calls fsync().
    //                        If false, only flushes the userspace buffer.
    WriteAheadLog(const std::filesystem::path& wal_directory,
                  bool sync_per_write);

    // Closes the write handle.
    ~WriteAheadLog();

    // Non-copyable, non-movable (owns a raw FILE* handle)
    WriteAheadLog(const WriteAheadLog&)            = delete;
    WriteAheadLog& operator=(const WriteAheadLog&) = delete;
    WriteAheadLog(WriteAheadLog&&)                 = delete;
    WriteAheadLog& operator=(WriteAheadLog&&)      = delete;

    // -----------------------------------------------------------------------
    // Core Operations
    // -----------------------------------------------------------------------

    // Serialize a KVRecord to binary, write it to the WAL file, and
    // optionally fsync. Throws std::runtime_error on I/O failure.
    void append(const KVRecord& record);

    // Read the entire WAL file and return all valid records in order.
    // Stops at the first record that fails CRC32 validation or is
    // truncated (partial write from a crash). Records after the
    // corruption point are silently discarded.
    //
    // This method opens a separate read handle and does NOT interfere
    // with the write handle used by append().
    [[nodiscard]]
    std::vector<KVRecord> replay() const;

    // Clear the WAL file (truncate to zero bytes). Called after a
    // successful SSTable flush to reclaim disk space.
    void truncate();

    // -----------------------------------------------------------------------
    // Observers
    // -----------------------------------------------------------------------

    // Current size of the WAL file in bytes.
    [[nodiscard]] size_t file_size() const;

    // Full path to the WAL file.
    [[nodiscard]] const std::filesystem::path& file_path() const noexcept;

private:
    // WAL file is always named "wal.log" inside the configured directory.
    static constexpr const char* kWALFilename = "wal.log";

    std::filesystem::path path_;           // Full path: wal_directory / wal.log
    bool                  sync_per_write_; // fsync on every append?
    FILE*                 write_handle_;   // Kept open in append mode ("ab")

    // -- File Handle Management ---------------------------------------------
    void open_for_append();
    void close_handle();

    // -- Sync ---------------------------------------------------------------
    // Flushes the C stream buffer. If sync_per_write_ is true, also
    // issues a platform-specific hardware sync:
    //   Windows: _fileno() + _commit()
    //   POSIX:   fileno()  + fsync()
    void sync_to_disk();

    // -- Serialization & CRC32 ----------------------------------------------

    // Serialize a KVRecord into a self-contained binary buffer with a
    // trailing CRC32 checksum. See wal.cpp for the detailed byte layout.
    static std::vector<uint8_t> serialize_record(const KVRecord& record);

    // Attempt to read and deserialize one record from the current file
    // position. Returns std::nullopt on EOF, short read, or CRC mismatch.
    static std::optional<KVRecord> deserialize_record(FILE* file);

    // Standard CRC32 using the reflected polynomial 0xEDB88320.
    // The lookup table is generated at compile time (constexpr).
    static uint32_t compute_crc32(const uint8_t* data, size_t length);
};

} // namespace kvault
