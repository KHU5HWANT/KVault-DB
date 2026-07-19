#pragma once

// ============================================================================
// sstable_writer.hpp — Sorted String Table Serializer
// ============================================================================
//
// Writes a frozen MemTable snapshot (a sorted vector<KVRecord>) to a
// self-describing binary .sst file on disk.
//
// The file is written sequentially from byte 0 to EOF in a single pass:
//
//   ┌──────────────────────────────────────────────────────────────────────┐
//   │  DATA BLOCK                                                          │
//   │  Sequential encoded key-value records, one after another            │
//   ├──────────────────────────────────────────────────────────────────────┤
//   │  SPARSE INDEX BLOCK                                                  │
//   │  Every Nth key + its data block offset — for binary-search lookup   │
//   ├──────────────────────────────────────────────────────────────────────┤
//   │  BLOOM FILTER BLOCK                                                  │
//   │  Serialized bit array (k, m, bits) from BloomFilter::serialize()     │
//   ├──────────────────────────────────────────────────────────────────────┤
//   │  FOOTER (fixed 48 bytes)                                             │
//   │  Block offsets, entry count, min/max key, magic number              │
//   └──────────────────────────────────────────────────────────────────────┘
//
// See sstable_writer.cpp for the exact byte-level layout of each section.
//
// ============================================================================

#include "kvault/bloom_filter.hpp"
#include "kvault/types.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace kvault {

class SSTableWriter {
public:
    // Sparse index granularity: one index entry per kIndexBlockInterval keys.
    // The reader binary-searches this index, then linearly scans within the
    // block. Smaller values → faster lookup, larger index block.
    static constexpr size_t kIndexBlockInterval = 100;

    // Magic number at the very end of the footer.
    // Hex ASCII for "KVLT" followed by version 0x0001.
    static constexpr uint64_t kMagicNumber = 0x544C564B00010000ULL;

    // -----------------------------------------------------------------------
    // Primary Interface
    // -----------------------------------------------------------------------

    // Write a sorted vector of KVRecords to a new SSTable file at `path`.
    // The entries MUST be sorted in ascending key order (produced by
    // MemTable::snapshot()).
    //
    // Steps:
    //   1. Write data block — all records sequentially
    //   2. Write sparse index block — every kIndexBlockInterval-th key
    //   3. Build and write Bloom Filter block
    //   4. Write fixed-size footer with block offsets
    //   5. fsync() for durability
    //
    // @param path             Destination .sst file path
    // @param sorted_entries   Sorted snapshot from MemTable::snapshot()
    // @param bits_per_key     Bloom Filter sizing (from EngineConfig)
    static void write(const std::filesystem::path& path,
                      const std::vector<KVRecord>& sorted_entries,
                      size_t bits_per_key = 10);

    SSTableWriter() = delete;  // Static-only class; not instantiable
};

// ============================================================================
// SSTableFooter — Fixed-size metadata block (48 bytes)
// ============================================================================
// Read first by the SSTableReader to locate all other blocks.
// Written last by the SSTableWriter after all variable blocks are on disk.
// ============================================================================
struct SSTableFooter {
    uint64_t index_block_offset;   // Byte offset of the Sparse Index Block
    uint64_t bloom_block_offset;   // Byte offset of the Bloom Filter Block
    uint64_t footer_offset;        // Byte offset of this footer itself
    uint64_t entry_count;          // Total number of records in the SSTable
    uint64_t data_block_size;      // Size of the Data Block in bytes
    uint64_t magic;                // kMagicNumber — validates file integrity

    // Serialized size of this struct (must always be exactly 48 bytes).
    static constexpr size_t kSerializedSize = 48;
};
static_assert(sizeof(SSTableFooter) == SSTableFooter::kSerializedSize,
              "SSTableFooter size mismatch — update kSerializedSize");

} // namespace kvault
