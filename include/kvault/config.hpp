#pragma once

// ============================================================================
// config.hpp — Tunable engine parameters for KVault
// ============================================================================
//
// All runtime-configurable knobs are centralized in EngineConfig.
// The KVStore constructor accepts an EngineConfig and distributes
// relevant fields to each subsystem (MemTable, WAL, SSTable, etc.).
//
// Defaults are chosen for a development/demo environment. Production
// deployments should tune memtable_flush_threshold_bytes and
// sync_per_write based on workload characteristics.
// ============================================================================

#include <cstddef>
#include <cstdint>
#include <string>

namespace kvault {

struct EngineConfig {

    // -- MemTable -----------------------------------------------------------

    // Approximate byte threshold at which the active MemTable is frozen
    // and flushed to a new SSTable on disk. Larger values increase write
    // throughput (fewer flushes) but consume more RAM.
    // Default: 4 MB
    size_t memtable_flush_threshold_bytes = 4UL * 1024 * 1024;

    // -- Skip List ----------------------------------------------------------

    // Maximum number of levels in the Skip List. A list with max_level = L
    // and probability = 0.5 efficiently handles up to 2^L entries.
    // Default: 16 (supports up to ~65K entries per MemTable)
    int skip_list_max_level = 16;

    // Probability of promoting a node to the next level during insertion.
    // p = 0.5 yields an expected 2 pointers per node (balanced tradeoff).
    // p = 0.25 uses ~1.33 pointers per node (more space-efficient).
    double skip_list_probability = 0.5;

    // -- Write-Ahead Log ----------------------------------------------------

    // Directory where the WAL file (wal.log) is stored.
    std::string wal_directory = "data/wal";

    // If true, every WAL append issues an fsync() / _commit() system call
    // to guarantee the record reaches stable storage before returning.
    //
    // If false, the WAL only flushes the userspace buffer (fflush). This
    // is faster but risks losing the last few records on a power failure
    // (the OS page cache may not have been written to disk).
    //
    // Recommendation:
    //   true  — correctness-critical workloads (default)
    //   false — benchmarking, development, or when using a UPS
    bool sync_per_write = true;

    // -- SSTables -----------------------------------------------------------

    // Directory where flushed SSTable files (.sst) are stored.
    std::string sstable_directory = "data/sstables";

    // -- Bloom Filter -------------------------------------------------------

    // Bits allocated per key in each SSTable's Bloom Filter.
    // 10 bits/key → ~1% false positive rate (k ≈ 7 hash functions).
    // Higher values reduce false positives but increase SSTable size.
    size_t bloom_filter_bits_per_key = 10;

    // -- HTTP Server --------------------------------------------------------

    // Port on which the embedded Crow HTTP server listens.
    uint16_t server_port = 8080;
};

} // namespace kvault
