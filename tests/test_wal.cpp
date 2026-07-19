// ============================================================================
// test_wal.cpp — Unit Tests for the Write-Ahead Log
// ============================================================================
//
// Tests the WAL's three critical guarantees:
//   1. DURABILITY  — append + replay recovers all records faithfully
//   2. INTEGRITY   — CRC32 checksums detect corrupted records
//   3. RESILIENCE  — partial writes (simulated crashes) are handled
//                    gracefully without crashing or returning bad data
//
// Each test uses a unique temporary directory that is cleaned up in
// TearDown(). This ensures test isolation even under parallel execution.
//
// ============================================================================

#include "kvault/wal.hpp"
#include "kvault/types.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace kvault {
namespace {

// ============================================================================
// Test Fixture — creates/destroys an isolated temp directory per test
// ============================================================================

class WALTest : public ::testing::Test {
protected:
    std::filesystem::path test_dir_;

    void SetUp() override {
        // Each test gets a unique directory based on the test name,
        // preventing interference between parallel test runs.
        const auto* info = ::testing::UnitTest::GetInstance()
                               ->current_test_info();
        test_dir_ = std::filesystem::temp_directory_path()
                    / ("kvault_wal_test_" + std::string(info->name()));

        // Clean up from any prior failed run
        std::filesystem::remove_all(test_dir_);
        std::filesystem::create_directories(test_dir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(test_dir_);
    }

    // Helper: full path to the WAL file inside the test directory.
    [[nodiscard]]
    std::filesystem::path wal_file_path() const {
        return test_dir_ / "wal.log";
    }
};

// ============================================================================
// Normal Replay — the core durability guarantee
// ============================================================================

TEST_F(WALTest, AppendAndReplayRecoversSingleRecord) {
    // Write one record, destroy the WAL, create a fresh one, replay.
    {
        WriteAheadLog wal(test_dir_, /*sync_per_write=*/false);
        wal.append({RecordType::PUT, "greeting", "hello"});
    }

    {
        WriteAheadLog wal(test_dir_, false);
        auto records = wal.replay();

        ASSERT_EQ(records.size(), 1u);
        EXPECT_EQ(records[0].type, RecordType::PUT);
        EXPECT_EQ(records[0].key, "greeting");
        EXPECT_EQ(records[0].value, "hello");
    }
}

TEST_F(WALTest, AppendAndReplayRecoversMultipleRecords) {
    constexpr int kNumRecords = 50;

    // Append many records
    {
        WriteAheadLog wal(test_dir_, false);
        for (int i = 0; i < kNumRecords; ++i) {
            wal.append({
                RecordType::PUT,
                "key_" + std::to_string(i),
                "value_" + std::to_string(i)
            });
        }
    }

    // Replay and verify every record is recovered in order
    {
        WriteAheadLog wal(test_dir_, false);
        auto records = wal.replay();

        ASSERT_EQ(records.size(), static_cast<size_t>(kNumRecords));
        for (int i = 0; i < kNumRecords; ++i) {
            EXPECT_EQ(records[static_cast<size_t>(i)].type, RecordType::PUT);
            EXPECT_EQ(records[static_cast<size_t>(i)].key,
                       "key_" + std::to_string(i));
            EXPECT_EQ(records[static_cast<size_t>(i)].value,
                       "value_" + std::to_string(i));
        }
    }
}

TEST_F(WALTest, ReplayDistinguishesPUTAndDELETERecords) {
    {
        WriteAheadLog wal(test_dir_, false);
        wal.append({RecordType::PUT,    "user",  "alice"});
        wal.append({RecordType::DELETE, "user",  ""});
        wal.append({RecordType::PUT,    "admin", "bob"});
    }

    {
        WriteAheadLog wal(test_dir_, false);
        auto records = wal.replay();

        ASSERT_EQ(records.size(), 3u);

        EXPECT_EQ(records[0].type, RecordType::PUT);
        EXPECT_EQ(records[0].key, "user");
        EXPECT_EQ(records[0].value, "alice");

        EXPECT_EQ(records[1].type, RecordType::DELETE);
        EXPECT_EQ(records[1].key, "user");
        EXPECT_TRUE(records[1].value.empty());

        EXPECT_EQ(records[2].type, RecordType::PUT);
        EXPECT_EQ(records[2].key, "admin");
        EXPECT_EQ(records[2].value, "bob");
    }
}

// ============================================================================
// Edge Cases — Empty WAL, Large Values
// ============================================================================

TEST_F(WALTest, ReplayOnFreshDirectoryReturnsEmptyVector) {
    // No WAL file exists yet — replay should return an empty vector
    // (not crash or throw).
    std::filesystem::path fresh_dir = test_dir_ / "subdir";
    WriteAheadLog wal(fresh_dir, false);
    auto records = wal.replay();

    EXPECT_TRUE(records.empty());
}

TEST_F(WALTest, LargeKeyAndValueArePreserved) {
    const std::string large_key(4096, 'K');    // 4 KB key
    const std::string large_val(65536, 'V');   // 64 KB value

    {
        WriteAheadLog wal(test_dir_, false);
        wal.append({RecordType::PUT, large_key, large_val});
    }

    {
        WriteAheadLog wal(test_dir_, false);
        auto records = wal.replay();

        ASSERT_EQ(records.size(), 1u);
        EXPECT_EQ(records[0].key.size(), 4096u);
        EXPECT_EQ(records[0].value.size(), 65536u);
        EXPECT_EQ(records[0].key, large_key);
        EXPECT_EQ(records[0].value, large_val);
    }
}

TEST_F(WALTest, EmptyKeyAndValueArePreserved) {
    {
        WriteAheadLog wal(test_dir_, false);
        wal.append({RecordType::PUT, "", ""});
    }

    {
        WriteAheadLog wal(test_dir_, false);
        auto records = wal.replay();

        ASSERT_EQ(records.size(), 1u);
        EXPECT_TRUE(records[0].key.empty());
        EXPECT_TRUE(records[0].value.empty());
    }
}

// ============================================================================
// Truncate — WAL is cleared after successful SSTable flush
// ============================================================================

TEST_F(WALTest, TruncateClearsAllRecords) {
    WriteAheadLog wal(test_dir_, false);
    wal.append({RecordType::PUT, "key1", "val1"});
    wal.append({RecordType::PUT, "key2", "val2"});

    EXPECT_GT(wal.file_size(), 0u);

    wal.truncate();

    EXPECT_EQ(wal.file_size(), 0u);

    // Replay after truncation should return nothing
    auto records = wal.replay();
    EXPECT_TRUE(records.empty());
}

TEST_F(WALTest, AppendWorksAfterTruncate) {
    WriteAheadLog wal(test_dir_, false);
    wal.append({RecordType::PUT, "old", "data"});
    wal.truncate();

    // Append new records after truncation
    wal.append({RecordType::PUT, "new", "data"});

    auto records = wal.replay();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].key, "new");
}

// ============================================================================
// File Size Tracking
// ============================================================================

TEST_F(WALTest, FileSizeGrowsWithEachAppend) {
    WriteAheadLog wal(test_dir_, false);

    const size_t initial = wal.file_size();
    wal.append({RecordType::PUT, "k1", "v1"});
    const size_t after_one = wal.file_size();
    wal.append({RecordType::PUT, "k2", "v2"});
    const size_t after_two = wal.file_size();

    EXPECT_EQ(initial, 0u);
    EXPECT_GT(after_one, 0u);
    EXPECT_GT(after_two, after_one);
}

// ============================================================================
// Corruption Handling — Simulated Crash Scenarios
// ============================================================================
//
// These tests are CRUCIAL for demonstrating crash-recovery correctness.
// They simulate what happens when the process crashes mid-write, leaving
// partial or garbage data at the end of the WAL file.
//
// Expectation: replay() must recover ALL complete, valid records and
// silently discard any corrupted tail — without crashing, throwing, or
// returning partial records.
// ============================================================================

TEST_F(WALTest, ReplayStopsAtGarbageBytes) {
    // Scenario: Two valid records are written. Then the process crashes
    // and leaves garbage bytes at the end (e.g., a partially overwritten
    // record from a concurrent flush, or filesystem corruption).

    // Step 1: Write 2 valid records
    {
        WriteAheadLog wal(test_dir_, false);
        wal.append({RecordType::PUT, "safe_1", "intact"});
        wal.append({RecordType::PUT, "safe_2", "intact"});
    }

    // Step 2: Append garbage bytes (simulating corruption)
    {
        std::ofstream ofs(wal_file_path(),
                          std::ios::binary | std::ios::app);
        ASSERT_TRUE(ofs.is_open()) << "Failed to open WAL for corruption";

        const char garbage[] = "\xDE\xAD\xBE\xEF_CORRUPTED_!@#$%^&*";
        ofs.write(garbage, sizeof(garbage) - 1);
        ofs.flush();
    }

    // Step 3: Replay — must recover the 2 valid records and stop
    {
        WriteAheadLog wal(test_dir_, false);
        auto records = wal.replay();

        ASSERT_EQ(records.size(), 2u)
            << "Should recover exactly 2 valid records before corruption";
        EXPECT_EQ(records[0].key, "safe_1");
        EXPECT_EQ(records[1].key, "safe_2");
    }
}

TEST_F(WALTest, ReplayStopsAtTruncatedRecord) {
    // Scenario: Three records are being written. The process crashes
    // mid-way through the 3rd record, leaving it partially written.
    // The first 2 records should be fully recoverable.

    size_t size_after_two = 0;

    // Step 1: Write 3 records, capturing the file size after record #2
    {
        WriteAheadLog wal(test_dir_, false);
        wal.append({RecordType::PUT, "rec_1", "complete"});
        wal.append({RecordType::PUT, "rec_2", "complete"});
        size_after_two = wal.file_size();

        wal.append({RecordType::PUT, "rec_3", "this_will_be_truncated"});
    }

    // Step 2: Simulate a crash by truncating the file mid-3rd-record.
    // We keep only the first 3 bytes of the 3rd record (just the type
    // byte and part of the key length — not enough for a valid record).
    ASSERT_GT(size_after_two, 0u);
    std::filesystem::resize_file(wal_file_path(), size_after_two + 3);

    // Step 3: Replay — must recover records 1 and 2, discard the partial 3rd
    {
        WriteAheadLog wal(test_dir_, false);
        auto records = wal.replay();

        ASSERT_EQ(records.size(), 2u)
            << "Should recover 2 complete records; 3rd is truncated";
        EXPECT_EQ(records[0].key, "rec_1");
        EXPECT_EQ(records[0].value, "complete");
        EXPECT_EQ(records[1].key, "rec_2");
        EXPECT_EQ(records[1].value, "complete");
    }
}

TEST_F(WALTest, ReplayStopsAtBadCRC) {
    // Scenario: A valid record has its CRC32 checksum corrupted.
    // This could happen due to a bit-flip in storage.

    // Step 1: Write 2 valid records
    {
        WriteAheadLog wal(test_dir_, false);
        wal.append({RecordType::PUT, "good", "record"});
        wal.append({RecordType::PUT, "will_be_corrupted", "data"});
    }

    // Step 2: Corrupt the CRC of the 2nd record by flipping a byte
    // near the end of the file. The last 4 bytes of the file are the
    // CRC32 of the 2nd record.
    {
        // Read the entire file
        std::ifstream ifs(wal_file_path(), std::ios::binary);
        std::string contents((std::istreambuf_iterator<char>(ifs)),
                              std::istreambuf_iterator<char>());
        ifs.close();

        ASSERT_GE(contents.size(), 4u);

        // Flip the last byte (part of the 2nd record's CRC)
        contents.back() = static_cast<char>(contents.back() ^ 0xFF);

        // Write back the corrupted file
        std::ofstream ofs(wal_file_path(),
                          std::ios::binary | std::ios::trunc);
        ofs.write(contents.data(),
                  static_cast<std::streamsize>(contents.size()));
        ofs.flush();
    }

    // Step 3: Replay — should recover only the 1st record (2nd has bad CRC)
    {
        WriteAheadLog wal(test_dir_, false);
        auto records = wal.replay();

        ASSERT_EQ(records.size(), 1u)
            << "Should recover 1 valid record; 2nd has corrupted CRC";
        EXPECT_EQ(records[0].key, "good");
        EXPECT_EQ(records[0].value, "record");
    }
}

TEST_F(WALTest, ReplayHandlesCompletelyCorruptedFile) {
    // Scenario: The WAL file is entirely garbage (e.g., filesystem
    // allocated the file but never wrote valid data before crashing).

    {
        std::ofstream ofs(wal_file_path(), std::ios::binary);
        const char noise[] = "\xFF\xFE\xFD\xFC\xFB\xFA\x00\x01\x02\x03";
        ofs.write(noise, sizeof(noise) - 1);
    }

    // Replay should return an empty vector — no valid records to recover.
    {
        WriteAheadLog wal(test_dir_, false);
        auto records = wal.replay();

        EXPECT_TRUE(records.empty())
            << "Completely corrupted WAL should yield zero records";
    }
}

// ============================================================================
// Sync Mode — verify both modes work without errors
// ============================================================================

TEST_F(WALTest, SyncPerWriteModeFunctionsCorrectly) {
    // This test verifies that sync_per_write=true doesn't crash or error.
    // We can't easily verify that fsync was called, but we can verify
    // the data is recoverable.
    WriteAheadLog wal(test_dir_, /*sync_per_write=*/true);
    wal.append({RecordType::PUT, "synced", "data"});

    auto records = wal.replay();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].key, "synced");
}

TEST_F(WALTest, BufferedModeFunctionsCorrectly) {
    WriteAheadLog wal(test_dir_, /*sync_per_write=*/false);
    wal.append({RecordType::PUT, "buffered", "data"});

    auto records = wal.replay();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].key, "buffered");
}

} // namespace
} // namespace kvault
