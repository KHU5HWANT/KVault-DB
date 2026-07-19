#pragma once

// ============================================================================
// bloom_filter.hpp — Space-Efficient Probabilistic Set Membership
// ============================================================================
//
// A Bloom Filter answers the question: "might this key be in the set?"
//
//   - FALSE  → the key is DEFINITELY not present (zero disk I/O on SSTable)
//   - TRUE   → the key is PROBABLY present (proceed to binary search)
//
// False positives are possible (rate ≈ (1 - e^(-k*n/m))^k).
// False negatives are IMPOSSIBLE — every key that was add()ed will always
// return true from might_contain().
//
// DOUBLE HASHING TECHNIQUE:
//   Instead of k independent hash functions (expensive to design/implement),
//   we use two base hashes h1 and h2 derived from the key, and generate
//   k composite hashes via:
//
//       g_i(key) = (h1(key) + i * h2(key)) mod m      for i in [0, k)
//
//   This is mathematically equivalent to k independent hashes for most
//   practical distributions [Kirsch & Mitzenmacher, 2006].
//
// PARAMETER GUIDANCE:
//   Given n keys and a target false positive rate p:
//     m = -n * ln(p) / (ln(2)^2)    — optimal bit count
//     k = (m / n) * ln(2)           — optimal hash count
//
//   With bits_per_key = 10:
//     p ≈ 0.0082  (~0.8% false positive rate)
//     k ≈ 7 hash functions
//
// SERIALIZATION:
//   The filter serializes to: [4 bytes: k] [4 bytes: m] [ceil(m/8) bytes: bits]
//   This self-contained format is embedded in the SSTable Bloom Filter Block.
//
// ============================================================================

#include "kvault/types.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace kvault {

class BloomFilter {
public:
    // -----------------------------------------------------------------------
    // Construction
    // -----------------------------------------------------------------------

    // Create a Bloom Filter sized for `expected_entries` keys with the
    // specified number of bits per key (default from EngineConfig).
    // @param expected_entries   Approximate number of keys to be inserted
    // @param bits_per_key       Controls the false positive rate
    //                           (10 → ~0.8%, 14 → ~0.1%, 20 → ~0.01%)
    BloomFilter(size_t expected_entries, size_t bits_per_key);

    // Reconstruct a BloomFilter from serialized bytes (used by SSTableReader).
    // @param data   Raw bytes produced by serialize()
    static BloomFilter deserialize(const std::vector<uint8_t>& data);

    ~BloomFilter() = default;
    BloomFilter(const BloomFilter&)            = default;
    BloomFilter& operator=(const BloomFilter&) = default;
    BloomFilter(BloomFilter&&) noexcept            = default;
    BloomFilter& operator=(BloomFilter&&) noexcept = default;

    // -----------------------------------------------------------------------
    // Core Operations
    // -----------------------------------------------------------------------

    // Add a key to the filter. Sets k bits derived from the key's two
    // base hashes via the double hashing formula.
    void add(const Key& key);

    // Test whether a key might be in the set.
    // Returns false  → key is DEFINITELY absent (safe to skip SSTable I/O)
    // Returns true   → key is PROBABLY present (proceed with binary search)
    [[nodiscard]]
    bool might_contain(const Key& key) const;

    // -----------------------------------------------------------------------
    // Serialization
    // -----------------------------------------------------------------------

    // Serialize the filter to a compact byte buffer for embedding in an
    // SSTable. Format:
    //   [0..3]  k (uint32 LE) — number of hash functions
    //   [4..7]  m (uint32 LE) — number of bits (capacity)
    //   [8..]   bit array     — ceil(m / 8) bytes, LSB first
    [[nodiscard]]
    std::vector<uint8_t> serialize() const;

    // -----------------------------------------------------------------------
    // Observers
    // -----------------------------------------------------------------------

    [[nodiscard]] size_t bit_count()  const noexcept { return m_; }
    [[nodiscard]] size_t hash_count() const noexcept { return k_; }
    [[nodiscard]] size_t byte_size()  const noexcept { return bits_.size(); }

private:
    // Private constructor used by deserialize()
    BloomFilter(size_t k, size_t m, std::vector<uint8_t> bits);

    // Compute the two base hashes for a key.
    // h1 is derived from FNV-1a, h2 from a complementary polynomial.
    // Both are deterministic and cheap to compute.
    static std::pair<uint64_t, uint64_t> base_hashes(const Key& key);

    // Set or test a single bit at position `bit_index` in bits_.
    void     set_bit(size_t bit_index);
    [[nodiscard]]
    bool     get_bit(size_t bit_index) const;

    size_t               k_;     // Number of hash functions
    size_t               m_;     // Total number of bits
    std::vector<uint8_t> bits_;  // Packed bit array: bits_[i/8] bit (i%8)
};

} // namespace kvault
