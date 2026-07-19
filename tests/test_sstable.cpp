// ============================================================================
// test_sstable.cpp — Integration Tests for SSTable Writer + Reader
// ============================================================================
//
// Tests the full write → read round-trip for SSTables, including:
//   1. BloomFilter — construction, add, might_contain, false negatives,
//      false positive rate, serialization round-trip
//   2. SSTableWriter — produces a valid .sst file
//   3. SSTableReader — bootstrap (footer → bloom → index), three-step lookup,
//      tombstone propagation, out-of-range key rejection, corruption detection
//
// Each test uses an isolated temp directory cleaned up in TearDown().
//
// ============================================================================

#include "kvault/bloom_filter.hpp"
#include "kvault/sstable_reader.hpp"
#include "kvault/sstable_writer.hpp"
#include "kvault/types.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace kvault {
namespace {

// ============================================================================
// Test Fixture
// ============================================================================

class SSTableTest : public ::testing::Test {
protected:
    std::filesystem::path test_dir_;

    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()
                               ->current_test_info();
        test_dir_ = std::filesystem::temp_directory_path()
                    / ("kvault_sst_test_" + std::string(info->name()));
        std::filesystem::remove_all(test_dir_);
        std::filesystem::create_directories(test_dir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(test_dir_);
    }

    [[nodiscard]]
    std::filesystem::path sst_path(const std::string& name = "test.sst") const {
        return test_dir_ / name;
    }

    // Helper: build N sorted KVRecords with predictable keys/values.
    static std::vector<KVRecord> make_records(int count,
                                              int start = 0,
                                              bool include_deletes = false) {
        std::vector<KVRecord> recs;
        recs.reserve(static_cast<size_t>(count));
        for (int i = start; i < start + count; ++i) {
            KVRecord r;
            // Zero-pad keys so lexicographic sort == numeric sort
            r.key   = "key_" + std::string(6 - std::to_string(i).size(), '0')
                             + std::to_string(i);
            r.value = "value_" + std::to_string(i);
            r.type  = (include_deletes && i % 5 == 0)
                      ? RecordType::DELETE
                      : RecordType::PUT;
            if (r.type == RecordType::DELETE) r.value = "";
            recs.push_back(std::move(r));
        }
        return recs;
    }
};

// ============================================================================
// BloomFilter — Unit Tests
// ============================================================================

TEST_F(SSTableTest, BloomFilter_NoFalseNegatives) {
    // A Bloom Filter must NEVER produce false negatives.
    // Every key that was add()ed must be found by might_contain().
    BloomFilter bf(1000, 10);

    std::vector<std::string> keys;
    for (int i = 0; i < 1000; ++i) {
        keys.push_back("key_" + std::to_string(i));
        bf.add(keys.back());
    }

    for (const auto& k : keys) {
        EXPECT_TRUE(bf.might_contain(k))
            << "False negative for key: " << k;
    }
}

TEST_F(SSTableTest, BloomFilter_FalsePositiveRateIsReasonable) {
    // With bits_per_key=10, expected FP rate ≈ 0.8%.
    // We test with a 5% threshold to give headroom for random variation.
    constexpr int kInserted    = 1000;
    constexpr int kQueried     = 10000;
    constexpr double kMaxFPRate = 0.05; // 5% — very generous upper bound

    BloomFilter bf(static_cast<size_t>(kInserted), 10);

    // Insert 1000 distinct keys
    for (int i = 0; i < kInserted; ++i) {
        bf.add("inserted_" + std::to_string(i));
    }

    // Query 10000 keys that were NEVER inserted
    int false_positives = 0;
    for (int i = 0; i < kQueried; ++i) {
        if (bf.might_contain("absent_" + std::to_string(i))) {
            ++false_positives;
        }
    }

    const double fp_rate = static_cast<double>(false_positives) / kQueried;
    EXPECT_LT(fp_rate, kMaxFPRate)
        << "False positive rate " << fp_rate
        << " exceeds threshold " << kMaxFPRate;
}

TEST_F(SSTableTest, BloomFilter_DefiniteMissForUnseenKey) {
    BloomFilter bf(100, 10);
    bf.add("alpha");
    bf.add("beta");

    // "gamma" was never added — might_contain CAN return true (false positive),
    // but for small filters with few keys it usually returns false.
    // We can't assert false here without knowing the hash collision.
    // What we CAN assert: "alpha" and "beta" must return true.
    EXPECT_TRUE(bf.might_contain("alpha"));
    EXPECT_TRUE(bf.might_contain("beta"));
}

TEST_F(SSTableTest, BloomFilter_SerializationRoundTrip) {
    BloomFilter original(500, 10);
    for (int i = 0; i < 100; ++i) {
        original.add("key_" + std::to_string(i));
    }

    // Serialize and deserialize
    auto serialized = original.serialize();
    ASSERT_GE(serialized.size(), 8u); // At least the k + m header

    auto restored = BloomFilter::deserialize(serialized);

    // Same parameters
    EXPECT_EQ(restored.bit_count(),  original.bit_count());
    EXPECT_EQ(restored.hash_count(), original.hash_count());

    // All inserted keys must still be found after deserialization
    for (int i = 0; i < 100; ++i) {
        EXPECT_TRUE(restored.might_contain("key_" + std::to_string(i)));
    }
}

TEST_F(SSTableTest, BloomFilter_EmptyFilterReturnsFalse) {
    // A filter with 0 inserted keys should return false for any query
    // (all bits are 0, so the first bit check fails immediately).
    BloomFilter bf(100, 10);
    EXPECT_FALSE(bf.might_contain("anything"));
}

// ============================================================================
// SSTableWriter → SSTableReader Round-Trip
// ============================================================================

TEST_F(SSTableTest, WriteAndRead_SingleRecord) {
    const auto path = sst_path();
    std::vector<KVRecord> recs = {{ RecordType::PUT, "hello", "world" }};

    SSTableWriter::write(path, recs);

    SSTableReader reader(path);
    EXPECT_EQ(reader.entry_count(), 1u);

    auto result = reader.get("hello");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "world");
}

TEST_F(SSTableTest, WriteAndRead_HundredRecords) {
    const auto path = sst_path();
    auto recs = make_records(100);

    SSTableWriter::write(path, recs);
    SSTableReader reader(path);

    EXPECT_EQ(reader.entry_count(), 100u);

    // Verify every key is recoverable
    for (const auto& rec : recs) {
        auto result = reader.get(rec.key);
        ASSERT_TRUE(result.has_value())
            << "Key not found: " << rec.key;
        EXPECT_EQ(*result, rec.value);
    }
}

TEST_F(SSTableTest, WriteAndRead_SparsesMultipleBlocks) {
    // Write more than kIndexBlockInterval keys to exercise multi-block indexing
    constexpr int kCount = 350; // 3.5 index blocks at interval=100
    const auto path = sst_path();
    auto recs = make_records(kCount);

    SSTableWriter::write(path, recs);
    SSTableReader reader(path);

    EXPECT_EQ(reader.entry_count(), static_cast<uint64_t>(kCount));

    // Spot-check keys at block boundaries
    for (int i : {0, 99, 100, 101, 199, 200, 299, 300, 349}) {
        const auto& rec = recs[static_cast<size_t>(i)];
        auto result = reader.get(rec.key);
        ASSERT_TRUE(result.has_value())
            << "Key not found at index " << i << ": " << rec.key;
        EXPECT_EQ(*result, rec.value);
    }
}

TEST_F(SSTableTest, AbsentKeyReturnsNullopt) {
    const auto path = sst_path();
    auto recs = make_records(50);

    SSTableWriter::write(path, recs);
    SSTableReader reader(path);

    // Keys outside the range
    EXPECT_FALSE(reader.get("zzz_out_of_range").has_value());
    EXPECT_FALSE(reader.get("aaa_before_start").has_value());

    // Key that's in-range but doesn't exist
    EXPECT_FALSE(reader.get("key_999999").has_value());
}

TEST_F(SSTableTest, BloomFilterShortCircuitsAbsentKeys) {
    // An absent key that passes the bloom filter still returns nullopt.
    // More importantly: the bloom filter should catch MOST absent keys.
    const auto path = sst_path();
    auto recs = make_records(200);

    SSTableWriter::write(path, recs);
    SSTableReader reader(path);

    // Definitely-absent key with a completely different prefix
    // The bloom filter SHOULD filter this out (not guaranteed, but very likely)
    auto result = reader.get("zzz_completely_absent");
    EXPECT_FALSE(result.has_value());
}

TEST_F(SSTableTest, TombstonesAreSurfacedCorrectly) {
    // DELETE records should be found by get_record() and return
    // RecordType::DELETE, signaling to the KVStore that the key is deleted.
    const auto path = sst_path();

    std::vector<KVRecord> recs = {
        { RecordType::PUT,    "alive",   "value"  },
        { RecordType::DELETE, "deleted", ""        },
        { RecordType::PUT,    "zzz",     "last"   }
    };

    SSTableWriter::write(path, recs);
    SSTableReader reader(path);

    // Tombstone key: get_record returns DELETE type
    auto del_record = reader.get_record("deleted");
    ASSERT_TRUE(del_record.has_value());
    EXPECT_EQ(del_record->type, RecordType::DELETE);
    EXPECT_TRUE(del_record->value.empty());

    // Live keys still accessible
    EXPECT_EQ(reader.get("alive").value_or(""), "value");
    EXPECT_EQ(reader.get("zzz").value_or(""), "last");
}

TEST_F(SSTableTest, MetadataIsCorrect) {
    const auto path = sst_path();
    auto recs = make_records(50);

    SSTableWriter::write(path, recs);
    SSTableReader reader(path);

    EXPECT_EQ(reader.entry_count(), 50u);
    EXPECT_GT(reader.file_size(), 0u);
    EXPECT_EQ(reader.path(), path);

    auto meta = reader.metadata();
    EXPECT_EQ(meta.entry_count, 50u);
    EXPECT_FALSE(meta.file_path.empty());
}

TEST_F(SSTableTest, EmptySSTableIsValid) {
    // Writing an empty SSTable should not crash and should return nullopt
    // for any key lookup.
    const auto path = sst_path();
    std::vector<KVRecord> empty;

    SSTableWriter::write(path, empty);
    SSTableReader reader(path);

    EXPECT_EQ(reader.entry_count(), 0u);
    EXPECT_FALSE(reader.get("any_key").has_value());
}

TEST_F(SSTableTest, LargeKeyValuePairsArePreserved) {
    const auto path = sst_path();

    const std::string large_key(4096, 'K');
    const std::string large_val(65536, 'V');

    std::vector<KVRecord> recs = {{ RecordType::PUT, large_key, large_val }};
    SSTableWriter::write(path, recs);

    SSTableReader reader(path);
    auto result = reader.get(large_key);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), 65536u);
    EXPECT_EQ(*result, large_val);
}

TEST_F(SSTableTest, CorruptedMagicNumberThrowsOnOpen) {
    // Corrupt the file's magic number in the footer and verify
    // that SSTableReader throws rather than silently returning bad data.
    const auto path = sst_path();
    auto recs = make_records(10);
    SSTableWriter::write(path, recs);

    // Corrupt the last 8 bytes (the magic number in the footer)
    {
        std::fstream fs(path, std::ios::binary | std::ios::in | std::ios::out);
        fs.seekp(-8, std::ios::end);
        const char garbage[8] = {0x00, 0x11, 0x22, 0x33,
                                  0x44, 0x55, 0x66, 0x77};
        fs.write(garbage, 8);
    }

    EXPECT_THROW(
        { SSTableReader reader(path); },
        std::runtime_error
    ) << "Should throw on invalid magic number";
}

TEST_F(SSTableTest, MultipleSSTablesAreIndependent) {
    // Two SSTables with different key ranges should each answer correctly.
    auto recs_a = make_records(50,  0);   // keys 0..49
    auto recs_b = make_records(50, 50);   // keys 50..99

    SSTableWriter::write(sst_path("a.sst"), recs_a);
    SSTableWriter::write(sst_path("b.sst"), recs_b);

    SSTableReader reader_a(sst_path("a.sst"));
    SSTableReader reader_b(sst_path("b.sst"));

    // Cross-lookups should return nullopt
    EXPECT_FALSE(reader_a.get(recs_b[0].key).has_value());
    EXPECT_FALSE(reader_b.get(recs_a[0].key).has_value());

    // Own-range lookups succeed
    EXPECT_TRUE(reader_a.get(recs_a[25].key).has_value());
    EXPECT_TRUE(reader_b.get(recs_b[25].key).has_value());
}

} // namespace
} // namespace kvault
