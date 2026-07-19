// ============================================================================
// test_kvstore_integration.cpp — End-to-end integration tests for KVStore
// ============================================================================
// Tests the full lifecycle of data moving from the Write-Ahead Log to the
// MemTable, triggering a flush based on size, writing an SSTable to disk,
// clearing the MemTable and WAL, and then reading back correctly from the
// SSTableManager fallback path.
// ============================================================================

#include "kvault/config.hpp"
#include "kvault/kvstore.hpp"

#include <gtest/gtest.h>
#include <filesystem>
#include <string>

namespace kvault {
namespace {

class KVStoreTest : public ::testing::Test {
protected:
    std::filesystem::path test_dir_;
    EngineConfig config_;

    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        test_dir_ = std::filesystem::temp_directory_path() / ("kvault_store_" + std::string(info->name()));
        std::filesystem::remove_all(test_dir_);
        std::filesystem::create_directories(test_dir_);

        config_.wal_directory = (test_dir_ / "wal").string();
        config_.sstable_directory = (test_dir_ / "sstables").string();
        // Set a small flush threshold to force flushes during tests
        config_.memtable_flush_threshold_bytes = 4096; // 4KB
        config_.sync_per_write = false; // faster tests
    }

    void TearDown() override {
        std::filesystem::remove_all(test_dir_);
    }
};

TEST_F(KVStoreTest, BasicPutAndGet) {
    KVStore store(config_);
    store.put("key1", "value1");
    store.put("key2", "value2");

    EXPECT_EQ(store.get("key1"), "value1");
    EXPECT_EQ(store.get("key2"), "value2");
    EXPECT_FALSE(store.get("key3").has_value());
}

TEST_F(KVStoreTest, FlushToSSTableOnThreshold) {
    KVStore store(config_);
    
    // 4KB threshold. We'll write ~1KB records until a flush occurs.
    std::string large_value(1024, 'A');

    EXPECT_EQ(store.sstable_count(), 0);

    for (int i = 0; i < 5; ++i) {
        store.put("large_key_" + std::to_string(i), large_value);
    }

    // Since we inserted 5 * ~1KB, the MemTable should have flushed at least once.
    EXPECT_GT(store.sstable_count(), 0);

    // The active MemTable size should be much smaller than the 5KB we inserted,
    // because it was cleared on flush.
    EXPECT_LT(store.memtable_size(), 4096);

    // Data should still be retrievable from the SSTable layer
    for (int i = 0; i < 5; ++i) {
        auto val = store.get("large_key_" + std::to_string(i));
        ASSERT_TRUE(val.has_value());
        EXPECT_EQ(*val, large_value);
    }
}

TEST_F(KVStoreTest, DeletesMaskSSTableValues) {
    KVStore store(config_);
    store.put("key1", "value1");
    store.force_flush();

    EXPECT_EQ(store.sstable_count(), 1);
    EXPECT_EQ(store.get("key1"), "value1"); // From SSTable

    // Now delete it
    store.remove("key1");

    // The tombstone is in the MemTable. It should mask the SSTable value.
    EXPECT_FALSE(store.get("key1").has_value());

    // Flush again to push the tombstone to an SSTable
    store.force_flush();
    EXPECT_EQ(store.sstable_count(), 2);

    // Should still return nullopt because the newest SSTable has a tombstone
    EXPECT_FALSE(store.get("key1").has_value());
}

TEST_F(KVStoreTest, RecoveryFromWAL) {
    // 1. Create a store, write data (don't flush)
    {
        KVStore store(config_);
        store.put("persist1", "val1");
        store.put("persist2", "val2");
        store.remove("persist1");
        // Data is in WAL and MemTable, but not SSTable
        EXPECT_EQ(store.sstable_count(), 0);
    } // store goes out of scope, shutting down. MemTable memory is lost, WAL remains.

    // 2. Re-open store with same config
    {
        KVStore store2(config_);
        
        // WAL should have been replayed into MemTable
        EXPECT_FALSE(store2.get("persist1").has_value()); // Was removed
        EXPECT_EQ(store2.get("persist2"), "val2"); // Was put
    }
}

} // namespace
} // namespace kvault
