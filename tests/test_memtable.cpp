// ============================================================================
// test_memtable.cpp — Unit Tests for the MemTable
// ============================================================================
//
// Tests the MemTable's three responsibilities:
//   1. Correct CRUD via Skip List delegation (including tombstones)
//   2. Approximate byte tracking and flush threshold detection
//   3. Sorted snapshot generation for SSTable flushing
//
// ============================================================================

#include "kvault/memtable.hpp"
#include "kvault/types.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace kvault {
namespace {

// Use a generous threshold so basic tests don't accidentally trigger a flush.
constexpr size_t kDefaultThreshold = 1024 * 1024; // 1 MB

// ============================================================================
// Basic PUT and GET
// ============================================================================

TEST(MemTableTest, PutAndGetReturnsCorrectValue) {
    MemTable mt(kDefaultThreshold);
    mt.put("name", "kvault");

    auto result = mt.get("name");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "kvault");
}

TEST(MemTableTest, GetNonExistentKeyReturnsNullopt) {
    MemTable mt(kDefaultThreshold);
    mt.put("exists", "yes");

    EXPECT_FALSE(mt.get("ghost").has_value());
}

TEST(MemTableTest, PutOverwritesPreviousValue) {
    MemTable mt(kDefaultThreshold);
    mt.put("version", "1.0");
    mt.put("version", "2.0");

    auto result = mt.get("version");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "2.0");
}

TEST(MemTableTest, MultipleDistinctKeys) {
    MemTable mt(kDefaultThreshold);
    mt.put("a", "alpha");
    mt.put("b", "beta");
    mt.put("c", "gamma");

    EXPECT_EQ(mt.entry_count(), 3u);
    EXPECT_EQ(mt.get("a").value_or(""), "alpha");
    EXPECT_EQ(mt.get("b").value_or(""), "beta");
    EXPECT_EQ(mt.get("c").value_or(""), "gamma");
}

// ============================================================================
// Tombstone Semantics
// ============================================================================

TEST(MemTableTest, DeleteMakesKeyReturnNullopt) {
    MemTable mt(kDefaultThreshold);
    mt.put("doomed", "value");
    mt.remove("doomed");

    // get() should return nullopt for tombstoned keys
    EXPECT_FALSE(mt.get("doomed").has_value());
}

TEST(MemTableTest, ContainsTombstoneReturnsTrueAfterDelete) {
    MemTable mt(kDefaultThreshold);
    mt.put("target", "alive");
    mt.remove("target");

    EXPECT_TRUE(mt.contains_tombstone("target"));
}

TEST(MemTableTest, ContainsTombstoneReturnsFalseForLiveKey) {
    MemTable mt(kDefaultThreshold);
    mt.put("healthy", "key");

    EXPECT_FALSE(mt.contains_tombstone("healthy"));
}

TEST(MemTableTest, ContainsTombstoneReturnsFalseForAbsentKey) {
    MemTable mt(kDefaultThreshold);
    EXPECT_FALSE(mt.contains_tombstone("never_inserted"));
}

TEST(MemTableTest, DeleteNonExistentKeyStillCreatesTombstone) {
    // This is critical: the key might exist in an older SSTable.
    // The tombstone must be inserted to shadow it during reads.
    MemTable mt(kDefaultThreshold);
    mt.remove("phantom");

    EXPECT_TRUE(mt.contains_tombstone("phantom"));
    EXPECT_FALSE(mt.get("phantom").has_value());
    EXPECT_EQ(mt.entry_count(), 1u);
}

TEST(MemTableTest, PutRevivesTombstonedKey) {
    MemTable mt(kDefaultThreshold);
    mt.put("phoenix", "v1");
    mt.remove("phoenix");

    // Key is tombstoned
    EXPECT_FALSE(mt.get("phoenix").has_value());
    EXPECT_TRUE(mt.contains_tombstone("phoenix"));

    // Revive it with a new value
    mt.put("phoenix", "v2");

    auto result = mt.get("phoenix");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "v2");
    EXPECT_FALSE(mt.contains_tombstone("phoenix"));
}

// ============================================================================
// Byte Tracking & Flush Threshold
// ============================================================================

TEST(MemTableTest, InitialSizeIsZero) {
    MemTable mt(kDefaultThreshold);
    EXPECT_EQ(mt.current_size_bytes(), 0u);
    EXPECT_FALSE(mt.should_flush());
}

TEST(MemTableTest, SizeIncreasesOnInsert) {
    MemTable mt(kDefaultThreshold);
    mt.put("key", "value");

    // Size should be at least key.size() + value.size() = 8 bytes.
    // Actual size includes per-node overhead, so it should be > 8.
    EXPECT_GT(mt.current_size_bytes(), 8u);
}

TEST(MemTableTest, UpsertWithLongerValueIncreasesSize) {
    MemTable mt(kDefaultThreshold);
    mt.put("key", "short");
    const size_t size_after_short = mt.current_size_bytes();

    mt.put("key", "a_much_longer_value_string");
    const size_t size_after_long = mt.current_size_bytes();

    // Replacing "short" (5 bytes) with a 26-byte value should increase size.
    EXPECT_GT(size_after_long, size_after_short);
}

TEST(MemTableTest, UpsertWithShorterValueDecreasesSize) {
    MemTable mt(kDefaultThreshold);
    mt.put("key", "a_very_long_value_string_here");
    const size_t size_before = mt.current_size_bytes();

    mt.put("key", "tiny");
    const size_t size_after = mt.current_size_bytes();

    EXPECT_LT(size_after, size_before);
}

TEST(MemTableTest, ShouldFlushTriggersAtConfiguredThreshold) {
    // Use a tiny threshold so we can trigger it with a few inserts.
    constexpr size_t tiny_threshold = 500;
    MemTable mt(tiny_threshold);

    // Insert entries until the threshold is exceeded.
    bool flushed = false;
    for (int i = 0; i < 100; ++i) {
        mt.put("key_" + std::to_string(i), "value_" + std::to_string(i));
        if (mt.should_flush()) {
            flushed = true;
            break;
        }
    }

    EXPECT_TRUE(flushed);
    EXPECT_GE(mt.current_size_bytes(), tiny_threshold);
}

TEST(MemTableTest, ShouldNotFlushBelowThreshold) {
    // Large threshold — a single insert should never trigger a flush.
    MemTable mt(1024 * 1024);
    mt.put("one_key", "one_value");

    EXPECT_FALSE(mt.should_flush());
}

// ============================================================================
// Snapshot — Sorted Dump for SSTable Flushing
// ============================================================================

TEST(MemTableTest, SnapshotReturnsRecordsInSortedKeyOrder) {
    MemTable mt(kDefaultThreshold);
    // Insert in deliberately unsorted order
    mt.put("cherry", "3");
    mt.put("apple", "1");
    mt.put("elderberry", "5");
    mt.put("banana", "2");
    mt.put("date", "4");

    auto records = mt.snapshot();

    ASSERT_EQ(records.size(), 5u);

    // Extract keys and verify sorted order
    std::vector<std::string> keys;
    keys.reserve(records.size());
    for (const auto& r : records) {
        keys.push_back(r.key);
    }
    EXPECT_TRUE(std::is_sorted(keys.begin(), keys.end()));

    // Verify the exact order
    EXPECT_EQ(records[0].key, "apple");
    EXPECT_EQ(records[4].key, "elderberry");
}

TEST(MemTableTest, SnapshotClassifiesLiveEntriesAsPUT) {
    MemTable mt(kDefaultThreshold);
    mt.put("alive", "kicking");

    auto records = mt.snapshot();

    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].type, RecordType::PUT);
    EXPECT_EQ(records[0].key, "alive");
    EXPECT_EQ(records[0].value, "kicking");
}

TEST(MemTableTest, SnapshotConvertsTombstonesToDELETERecords) {
    MemTable mt(kDefaultThreshold);
    mt.put("doomed", "value");
    mt.remove("doomed");

    auto records = mt.snapshot();

    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].type, RecordType::DELETE);
    EXPECT_EQ(records[0].key, "doomed");
    EXPECT_TRUE(records[0].value.empty()) << "DELETE records should have empty value";
}

TEST(MemTableTest, SnapshotMixesLiveAndDeletedEntries) {
    MemTable mt(kDefaultThreshold);
    mt.put("a", "alive");
    mt.put("b", "also_alive");
    mt.remove("b");
    mt.put("c", "still_alive");

    auto records = mt.snapshot();

    ASSERT_EQ(records.size(), 3u);

    // "a" — live
    EXPECT_EQ(records[0].type, RecordType::PUT);
    EXPECT_EQ(records[0].key, "a");

    // "b" — tombstoned
    EXPECT_EQ(records[1].type, RecordType::DELETE);
    EXPECT_EQ(records[1].key, "b");
    EXPECT_TRUE(records[1].value.empty());

    // "c" — live
    EXPECT_EQ(records[2].type, RecordType::PUT);
    EXPECT_EQ(records[2].key, "c");
}

TEST(MemTableTest, SnapshotOfEmptyMemTableIsEmpty) {
    MemTable mt(kDefaultThreshold);
    auto records = mt.snapshot();
    EXPECT_TRUE(records.empty());
}

// ============================================================================
// Entry Count
// ============================================================================

TEST(MemTableTest, EntryCountTracksUniqueKeys) {
    MemTable mt(kDefaultThreshold);
    mt.put("a", "1");
    mt.put("b", "2");
    mt.put("a", "3");  // Upsert — should NOT increase count

    EXPECT_EQ(mt.entry_count(), 2u);
}

TEST(MemTableTest, EntryCountIncludesTombstonedKeys) {
    MemTable mt(kDefaultThreshold);
    mt.put("live", "yes");
    mt.remove("dead_on_arrival");  // Tombstone for non-existent key

    // Both the live key and the tombstone count as entries
    EXPECT_EQ(mt.entry_count(), 2u);
}

} // namespace
} // namespace kvault
