#include "kvault/memtable.hpp"

#include <string>

namespace kvault {

// ============================================================================
// Construction
// ============================================================================

MemTable::MemTable(size_t flush_threshold_bytes)
    : approximate_size_(0)
    , flush_threshold_bytes_(flush_threshold_bytes)
{}

// ============================================================================
// put() — Insert or Upsert
// ============================================================================
//
// Byte tracking strategy:
//   - NEW key:    approximate_size_ += key.size() + value.size() + overhead
//   - UPSERT:     approximate_size_ += (new_value.size() - old_value.size())
//                 (key size and node overhead are unchanged on upsert)
//
// If the key was previously tombstoned, the tombstone value size is
// subtracted and the new value size is added. The key "comes back to life."
// ============================================================================

void MemTable::put(const Key& key, const Value& value) {
    auto existing = skiplist_.search(key);

    if (existing.has_value()) {
        // Upsert: key and node overhead are unchanged.
        // Only adjust for the value size delta.
        approximate_size_ -= existing->size();
        approximate_size_ += value.size();
    } else {
        // New entry: account for everything.
        approximate_size_ += key.size() + value.size() + kNodeOverheadBytes;
    }

    skiplist_.insert(key, value);
}

// ============================================================================
// remove() — Tombstone Insertion
// ============================================================================
//
// Deletes are NOT physical removals from the Skip List. Instead, we insert
// the key with the special tombstone sentinel value. This is necessary
// because:
//
//   1. The key might exist in an older SSTable on disk. Without a tombstone
//      in the MemTable, the read path would fall through and "find" the
//      key in the SSTable — returning stale data.
//
//   2. When the MemTable is flushed to an SSTable, the tombstone is
//      preserved as a DELETE record, ensuring the deletion propagates
//      through compaction.
// ============================================================================

void MemTable::remove(const Key& key) {
    auto existing = skiplist_.search(key);

    if (existing.has_value()) {
        // Key exists (live or already tombstoned): replace its value.
        approximate_size_ -= existing->size();
        approximate_size_ += kTombstoneValue.size();
    } else {
        // Key not in this MemTable — insert a new tombstone entry.
        approximate_size_ += key.size() + kTombstoneValue.size()
                           + kNodeOverheadBytes;
    }

    skiplist_.insert(key, std::string(kTombstoneValue));
}

// ============================================================================
// get() — Read with Tombstone Filtering
// ============================================================================
//
// Returns std::nullopt in two cases:
//   1. Key not found in the Skip List
//   2. Key found but its value is the tombstone sentinel
//
// The caller (KVStore) must distinguish these cases using
// contains_tombstone() to decide whether to search SSTables.
// ============================================================================

std::optional<Value> MemTable::get(const Key& key) const {
    auto result = skiplist_.search(key);

    // Filter out tombstones — they represent deleted keys.
    if (result.has_value() && *result == kTombstoneValue) {
        return std::nullopt;
    }

    return result;
}

// ============================================================================
// contains_tombstone() — Explicit Tombstone Check
// ============================================================================
//
// Used by the KVStore read path to short-circuit SSTable lookups:
//
//   if (memtable.contains_tombstone(key)) {
//       // Key is definitively deleted — don't search SSTables
//       return std::nullopt;
//   }
//
// Without this, a "not found" from get() would cause an unnecessary
// (and potentially expensive) SSTable search for a key that has been
// explicitly deleted.
// ============================================================================

bool MemTable::contains_tombstone(const Key& key) const {
    auto result = skiplist_.search(key);
    return result.has_value() && *result == kTombstoneValue;
}

// ============================================================================
// Size & Threshold Observers
// ============================================================================

size_t MemTable::current_size_bytes() const noexcept {
    return approximate_size_;
}

bool MemTable::should_flush() const noexcept {
    return approximate_size_ >= flush_threshold_bytes_;
}

size_t MemTable::entry_count() const noexcept {
    return skiplist_.size();
}

// ============================================================================
// snapshot() — Ordered Dump for SSTable Flushing
// ============================================================================
//
// Walks the Skip List's level-0 chain (which is sorted by key) and
// builds a vector of KVRecords:
//
//   - Live entries (value != tombstone) → RecordType::PUT
//   - Tombstoned entries               → RecordType::DELETE (value = "")
//
// The returned vector is guaranteed to be in ascending key order,
// which is exactly what the SSTable writer requires.
// ============================================================================

std::vector<KVRecord> MemTable::snapshot() const {
    std::vector<KVRecord> records;
    records.reserve(skiplist_.size());

    for (auto it = skiplist_.begin(); it != skiplist_.end(); ++it) {
        KVRecord record;

        if (it.value() == kTombstoneValue) {
            record.type  = RecordType::DELETE;
            record.key   = it.key();
            // record.value is left default-constructed (empty string)
        } else {
            record.type  = RecordType::PUT;
            record.key   = it.key();
            record.value = it.value();
        }

        records.push_back(std::move(record));
    }

    return records;
}

} // namespace kvault
