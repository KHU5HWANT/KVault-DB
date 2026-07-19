#pragma once

#include "kvault/config.hpp"
#include "kvault/memtable.hpp"
#include "kvault/sstable_manager.hpp"
#include "kvault/types.hpp"
#include "kvault/wal.hpp"

#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>

namespace kvault {

class KVStore {
public:
    explicit KVStore(const EngineConfig& config = EngineConfig{});
    ~KVStore() = default;

    // Non-copyable, non-movable for safety
    KVStore(const KVStore&) = delete;
    KVStore& operator=(const KVStore&) = delete;

    // Insert or update a key-value pair.
    void put(const Key& key, const Value& value);

    // Retrieve a value by key. Returns std::nullopt if not found.
    std::optional<Value> get(const Key& key) const;

    // Delete a key. Returns true if key was logically removed.
    bool remove(const Key& key);

    // Flushes MemTable to disk manually, creating a new SSTable.
    void force_flush();

    // Diagnostics/Metrics
    size_t memtable_size() const;
    size_t sstable_count() const;
    size_t wal_size() const;

    // Returns a sorted snapshot of the active MemTable for the dashboard visualizer.
    // Each entry is a pair { key, value } — tombstones are included with value=<TOMBSTONE>.
    std::vector<KVRecord> memtable_snapshot() const;

private:
    void trigger_flush();

    EngineConfig config_;
    
    // We use a shared_mutex to allow concurrent reads from MemTable/SSTables,
    // while acquiring an exclusive lock during flushes.
    // (Individual put() and get() calls are fast, so for now we exclusively lock
    // on put/remove to simplify concurrent access to the WAL and MemTable. A real
    // high-concurrency engine would use a concurrent SkipList).
    mutable std::shared_mutex rw_mutex_;

    std::unique_ptr<WriteAheadLog> wal_;
    std::unique_ptr<MemTable> memtable_;
    std::unique_ptr<SSTableManager> sstable_manager_;
};

} // namespace kvault
