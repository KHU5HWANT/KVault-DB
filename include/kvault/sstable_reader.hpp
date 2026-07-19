#pragma once

// ============================================================================
// sstable_reader.hpp — Sorted String Table Lookup Engine
// ============================================================================
//
// Provides O(log n) point lookups into an .sst file without loading the
// entire data block into memory. Only the Footer, Sparse Index, and Bloom
// Filter are kept in RAM after construction.
//
// READ PATH for get(key):
//
//   Step 1 — Bloom Filter Check (pure in-memory, zero disk I/O)
//            If might_contain(key) == false → return nullopt immediately
//
//   Step 2 — Sparse Index Binary Search (pure in-memory)
//            Find the two index entries bracketing the target key.
//            This yields a [start_offset, end_offset) range in the data block.
//
//   Step 3 — Data Block Scan (1 disk seek + sequential read of ≤ N records)
//            fseek() to start_offset, read records one by one until key found
//            or end_offset is reached.
//
// The worst-case disk I/O is reading kIndexBlockInterval sequential records
// (100 by default), which fits in 1–3 disk pages for typical KV sizes.
//
// ============================================================================

#include "kvault/bloom_filter.hpp"
#include "kvault/sstable_writer.hpp"  // For SSTableFooter, kMagicNumber
#include "kvault/types.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace kvault {

// One entry in the in-memory sparse index.
// Maps a key boundary to its byte offset in the data block.
struct IndexEntry {
    Key      key;    // First key in this sparse block
    uint64_t offset; // Byte offset of this key's record in the data block
};

// Metadata exposed to the SSTableManager without opening the file.
struct SSTableMetadata {
    std::string   min_key;
    std::string   max_key;
    uint64_t      entry_count;
    uint64_t      file_size_bytes;
    std::string   file_path;
};

class SSTableReader {
public:
    // -----------------------------------------------------------------------
    // Construction
    // -----------------------------------------------------------------------

    // Open an SSTable file and load its footer, index, and bloom filter
    // into memory. Throws std::runtime_error if the file is missing,
    // unreadable, or has a bad magic number.
    explicit SSTableReader(const std::filesystem::path& path);

    // Destructor closes the file handle.
    ~SSTableReader();

    // Non-copyable (owns a FILE* handle and heap-allocated index/filter).
    SSTableReader(const SSTableReader&)            = delete;
    SSTableReader& operator=(const SSTableReader&) = delete;

    // Movable.
    SSTableReader(SSTableReader&&) noexcept;
    SSTableReader& operator=(SSTableReader&&) noexcept;

    // -----------------------------------------------------------------------
    // Core Read Operation
    // -----------------------------------------------------------------------

    // Look up a key in this SSTable.
    //
    // Returns:
    //   std::optional<Value> with the value   — key found as a PUT record
    //   std::optional<Value> with ""           — key is a DELETE tombstone
    //                                            (caller must check RecordType)
    //   std::nullopt                           — key not in this SSTable
    //
    // Note: To distinguish between "not found" and "tombstone", callers
    // should use get_record() or check the returned value against the
    // tombstone sentinel. The KVStore read path handles this logic.
    [[nodiscard]]
    std::optional<Value> get(const Key& key) const;

    // Full record lookup — returns the KVRecord (including RecordType) if
    // the key is found. Returns nullopt if not found.
    [[nodiscard]]
    std::optional<KVRecord> get_record(const Key& key) const;

    // -----------------------------------------------------------------------
    // Metadata & Observers
    // -----------------------------------------------------------------------

    [[nodiscard]] const std::string& min_key()     const noexcept;
    [[nodiscard]] const std::string& max_key()     const noexcept;
    [[nodiscard]] uint64_t           entry_count() const noexcept;
    [[nodiscard]] uint64_t           file_size()   const;
    [[nodiscard]] const std::filesystem::path& path() const noexcept;

    // Returns a snapshot of this SSTable's metadata for the Manager/API.
    [[nodiscard]] SSTableMetadata metadata() const;

private:
    // -----------------------------------------------------------------------
    // Internal helpers
    // -----------------------------------------------------------------------

    // Read and validate the 48-byte footer from the end of the file.
    SSTableFooter read_footer() const;

    // Load the sparse index into memory from the index block.
    void load_index(uint64_t index_block_offset,
                    uint64_t bloom_block_offset);

    // Load the Bloom Filter into memory from the bloom block.
    void load_bloom_filter(uint64_t bloom_block_offset,
                           uint64_t footer_offset);

    // Read a single KVRecord from the current file position.
    // Returns false on EOF or read error.
    static bool read_record(FILE* file, KVRecord& out);

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------
    std::filesystem::path path_;
    mutable FILE*         file_;        // Kept open for seek+read in get()

    // In-memory index (small: one entry per kIndexBlockInterval keys)
    std::vector<IndexEntry> index_;

    // In-memory Bloom Filter
    BloomFilter bloom_filter_;

    // Cached footer fields
    uint64_t    entry_count_;
    uint64_t    data_block_size_;       // Size of data block in bytes
    uint64_t    index_block_offset_;    // Absolute file offset of index block
    std::string min_key_;
    std::string max_key_;
};

} // namespace kvault
