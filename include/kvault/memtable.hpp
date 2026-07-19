#pragma once

// ============================================================================
// memtable.hpp — In-Memory Sorted Store (MemTable)
// ============================================================================
//
// The MemTable is a thin encapsulation over the Skip List that adds:
//
//   1. BYTE TRACKING — maintains an approximate running count of memory
//      usage to determine when the MemTable should be flushed to disk.
//
//   2. TOMBSTONE SEMANTICS — translates user-facing "delete" operations
//      into internal tombstone insertions, and filters them out on reads.
//
//   3. SNAPSHOT — produces a sorted vector of KVRecords for the SSTable
//      writer to serialize during the flush pipeline.
//
// Lifecycle:
//   Active MemTable (accepting writes)
//     → should_flush() returns true
//     → KVStore swaps it to "immutable" status
//     → snapshot() is called by the flush pipeline
//     → SSTable is written to disk
//     → MemTable is destroyed (arena releases all nodes)
//
// Thread Safety: NOT thread-safe. The KVStore layer is responsible for
//                synchronization (e.g., a read-write lock).
// ============================================================================

#include "kvault/skiplist.hpp"
#include "kvault/types.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace kvault {

class MemTable {
public:
    // -----------------------------------------------------------------------
    // Construction
    // -----------------------------------------------------------------------
    // @param flush_threshold_bytes  Approximate byte count at which
    //                               should_flush() begins returning true.
    //                               Sourced from EngineConfig.
    // -----------------------------------------------------------------------
    explicit MemTable(size_t flush_threshold_bytes);

    ~MemTable() = default;

    // Non-copyable (SkipList owns an arena of unique_ptrs)
    MemTable(const MemTable&)            = delete;
    MemTable& operator=(const MemTable&) = delete;

    // Movable (SkipList is movable)
    MemTable(MemTable&&) noexcept            = default;
    MemTable& operator=(MemTable&&) noexcept = default;

    // -----------------------------------------------------------------------
    // Write Operations
    // -----------------------------------------------------------------------

    // Insert or update a key-value pair.
    // If the key was previously tombstoned in this MemTable, the tombstone
    // is replaced with the new value (the key "comes back to life").
    void put(const Key& key, const Value& value);

    // Mark a key as deleted by inserting a tombstone sentinel.
    // The tombstone is necessary even if the key doesn't exist in this
    // MemTable — it might exist in an older SSTable on disk, and the
    // tombstone must shadow it during the read path.
    void remove(const Key& key);

    // -----------------------------------------------------------------------
    // Read Operations
    // -----------------------------------------------------------------------

    // Retrieve the value for a key.
    //
    // Returns:
    //   std::optional<Value> with the value — if the key exists and is live
    //   std::nullopt — if the key is not found OR is tombstoned
    //
    // Callers cannot distinguish "not found" from "deleted" at this layer.
    // The KVStore read path handles this by falling through to SSTables
    // only when the MemTable returns nullopt AND the key was NOT tombstoned.
    // See: contains_tombstone() for explicit tombstone checks.
    [[nodiscard]]
    std::optional<Value> get(const Key& key) const;

    // Check if a key has an explicit tombstone in this MemTable.
    // Used by KVStore to avoid unnecessary SSTable lookups: if the
    // MemTable has a tombstone for a key, the key is definitively deleted
    // and there's no need to search older SSTables.
    [[nodiscard]]
    bool contains_tombstone(const Key& key) const;

    // -----------------------------------------------------------------------
    // Size & Threshold
    // -----------------------------------------------------------------------

    // Approximate byte size of all entries in this MemTable.
    // Accounts for key bytes, value bytes, and estimated per-node overhead
    // from the underlying Skip List (forward pointers, arena storage).
    [[nodiscard]] size_t current_size_bytes() const noexcept;

    // Returns true when the MemTable has exceeded its configured flush
    // threshold and should be swapped to immutable + flushed to an SSTable.
    [[nodiscard]] bool should_flush() const noexcept;

    // Number of unique keys in the MemTable (includes tombstoned keys).
    [[nodiscard]] size_t entry_count() const noexcept;

    // -----------------------------------------------------------------------
    // Snapshot (for SSTable Flushing)
    // -----------------------------------------------------------------------

    // Returns a sorted vector of all entries as KVRecords.
    //
    // - Live entries → KVRecord { RecordType::PUT, key, value }
    // - Tombstones   → KVRecord { RecordType::DELETE, key, "" }
    //
    // The vector is ordered by key (ascending, lexicographic) because
    // SSTables require sorted input. This is achieved by iterating the
    // Skip List's level-0 chain.
    [[nodiscard]]
    std::vector<KVRecord> snapshot() const;

private:
    // Estimated overhead per Skip List node, in bytes.
    //
    // Breakdown:
    //   SkipListNode struct (2 std::strings + vector): ~64 bytes
    //   Average forward pointers (p=0.5 → ~2 levels × 8B): ~16 bytes
    //   unique_ptr in arena vector: 8 bytes
    //   Heap allocator metadata: ~16 bytes
    //   ─────────────────────────────────────────────────────
    //   Total: ~104 bytes
    //
    // This is approximate. The flush threshold should be set with the
    // understanding that actual memory usage ≈ 2× the tracked byte count.
    static constexpr size_t kNodeOverheadBytes = 104;

    SkipList skiplist_;
    size_t   approximate_size_;
    size_t   flush_threshold_bytes_;
};

} // namespace kvault
