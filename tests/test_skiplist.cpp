// ============================================================================
// test_skiplist.cpp — Unit Tests for the Custom Skip List
// ============================================================================
//
// Validates the Skip List's core operations: insert, search, remove, upsert,
// and in-order iteration. Includes a stress test with 10,000 random entries.
//
// These tests exercise the arena-based ownership model — if memory
// management is broken, sanitizers (ASan/MSan) will catch it here.
// ============================================================================

#include "kvault/skiplist.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace kvault {
namespace {

// ============================================================================
// Basic Operations
// ============================================================================

TEST(SkipListTest, InsertAndSearchSingleKey) {
    SkipList sl;
    sl.insert("hello", "world");

    auto result = sl.search("hello");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "world");
    EXPECT_EQ(sl.size(), 1u);
}

TEST(SkipListTest, InsertMultipleKeysAndSearchEach) {
    SkipList sl;
    sl.insert("alpha", "1");
    sl.insert("beta", "2");
    sl.insert("gamma", "3");

    EXPECT_EQ(sl.search("alpha").value_or(""), "1");
    EXPECT_EQ(sl.search("beta").value_or(""), "2");
    EXPECT_EQ(sl.search("gamma").value_or(""), "3");
    EXPECT_EQ(sl.size(), 3u);
}

TEST(SkipListTest, SearchNonExistentKeyReturnsNullopt) {
    SkipList sl;
    sl.insert("exists", "yes");

    auto result = sl.search("ghost");
    EXPECT_FALSE(result.has_value());
}

TEST(SkipListTest, SearchOnEmptyListReturnsNullopt) {
    SkipList sl;
    EXPECT_FALSE(sl.search("anything").has_value());
}

// ============================================================================
// Upsert (Insert-or-Update) Semantics
// ============================================================================

TEST(SkipListTest, UpsertOverwritesExistingValue) {
    SkipList sl;
    sl.insert("key", "original");
    sl.insert("key", "updated");

    auto result = sl.search("key");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "updated");
}

TEST(SkipListTest, UpsertDoesNotChangeSizeCount) {
    SkipList sl;
    sl.insert("key", "v1");
    sl.insert("key", "v2");
    sl.insert("key", "v3");

    EXPECT_EQ(sl.size(), 1u);
}

// ============================================================================
// Removal
// ============================================================================

TEST(SkipListTest, RemoveExistingKeyReturnsTrue) {
    SkipList sl;
    sl.insert("target", "value");

    EXPECT_TRUE(sl.remove("target"));
    EXPECT_FALSE(sl.search("target").has_value());
    EXPECT_EQ(sl.size(), 0u);
}

TEST(SkipListTest, RemoveNonExistentKeyReturnsFalse) {
    SkipList sl;
    sl.insert("a", "1");

    EXPECT_FALSE(sl.remove("b"));
    EXPECT_EQ(sl.size(), 1u);
}

TEST(SkipListTest, RemoveFromEmptyListReturnsFalse) {
    SkipList sl;
    EXPECT_FALSE(sl.remove("phantom"));
}

TEST(SkipListTest, RemovedKeyIsNotFoundBySearch) {
    SkipList sl;
    sl.insert("x", "100");
    sl.insert("y", "200");
    sl.insert("z", "300");

    sl.remove("y");

    EXPECT_TRUE(sl.search("x").has_value());
    EXPECT_FALSE(sl.search("y").has_value());
    EXPECT_TRUE(sl.search("z").has_value());
    EXPECT_EQ(sl.size(), 2u);
}

// ============================================================================
// Sorted Iteration (Level-0 Chain)
// ============================================================================

TEST(SkipListTest, IteratorYieldsKeysInAscendingOrder) {
    SkipList sl;
    // Deliberately unsorted insertion order
    sl.insert("cherry", "3");
    sl.insert("apple", "1");
    sl.insert("elderberry", "5");
    sl.insert("banana", "2");
    sl.insert("date", "4");

    std::vector<std::string> keys;
    for (auto it = sl.begin(); it != sl.end(); ++it) {
        keys.push_back(it.key());
    }

    ASSERT_EQ(keys.size(), 5u);
    EXPECT_TRUE(std::is_sorted(keys.begin(), keys.end()));
    EXPECT_EQ(keys[0], "apple");
    EXPECT_EQ(keys[4], "elderberry");
}

TEST(SkipListTest, IteratorOnEmptyListProducesNothing) {
    SkipList sl;
    EXPECT_EQ(sl.begin(), sl.end());

    int count = 0;
    for (auto it = sl.begin(); it != sl.end(); ++it) {
        ++count;
    }
    EXPECT_EQ(count, 0);
}

TEST(SkipListTest, IteratorReflectsRemovals) {
    SkipList sl;
    sl.insert("a", "1");
    sl.insert("b", "2");
    sl.insert("c", "3");

    sl.remove("b");

    std::vector<std::string> keys;
    for (auto it = sl.begin(); it != sl.end(); ++it) {
        keys.push_back(it.key());
    }

    ASSERT_EQ(keys.size(), 2u);
    EXPECT_EQ(keys[0], "a");
    EXPECT_EQ(keys[1], "c");
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST(SkipListTest, EmptyStringKeysAndValues) {
    SkipList sl;
    sl.insert("", "empty_key");
    sl.insert("empty_val", "");

    EXPECT_EQ(sl.search("").value_or("FAIL"), "empty_key");
    EXPECT_EQ(sl.search("empty_val").value_or("FAIL"), "");
    EXPECT_EQ(sl.size(), 2u);
}

TEST(SkipListTest, EmptyAfterRemovingAllKeys) {
    SkipList sl;
    sl.insert("a", "1");
    sl.insert("b", "2");

    sl.remove("a");
    sl.remove("b");

    EXPECT_TRUE(sl.empty());
    EXPECT_EQ(sl.size(), 0u);
    EXPECT_EQ(sl.begin(), sl.end());
}

// ============================================================================
// Stress Test — validates correctness at scale and exercises the arena
// ============================================================================

TEST(SkipListTest, StressTest_10KRandomInserts) {
    SkipList sl;
    constexpr int kNumEntries = 10000;
    std::set<std::string> reference_keys;

    // Insert keys in a scrambled order using modular arithmetic
    for (int i = 0; i < kNumEntries; ++i) {
        std::string key = "key_" + std::to_string((i * 7919) % kNumEntries);
        std::string val = "val_" + std::to_string(i);
        sl.insert(key, val);
        reference_keys.insert(key);
    }

    // Verify size matches the number of unique keys
    EXPECT_EQ(sl.size(), reference_keys.size());

    // Verify every key is searchable
    for (const auto& key : reference_keys) {
        EXPECT_TRUE(sl.search(key).has_value()) << "Missing key: " << key;
    }

    // Verify iteration order matches std::set (which is also sorted)
    auto ref_it = reference_keys.begin();
    for (auto it = sl.begin(); it != sl.end(); ++it, ++ref_it) {
        ASSERT_NE(ref_it, reference_keys.end());
        EXPECT_EQ(it.key(), *ref_it);
    }
    EXPECT_EQ(ref_it, reference_keys.end());
}

} // namespace
} // namespace kvault
