#include "kvault/bloom_filter.hpp"

#include <cmath>
#include <cstring>
#include <stdexcept>

namespace kvault {

// ============================================================================
// Construction
// ============================================================================

BloomFilter::BloomFilter(size_t expected_entries, size_t bits_per_key) {
    // m = total bit count = expected_entries * bits_per_key
    // Clamp to at least 64 bits to avoid edge-case division issues.
    m_ = std::max(expected_entries * bits_per_key, size_t{64});

    // k = optimal hash count = bits_per_key * ln(2) ≈ bits_per_key * 0.693
    // Clamp between 1 and 30 (30 hashes is already overkill at any bit count).
    const double k_ideal = static_cast<double>(bits_per_key) * 0.693147;
    k_ = static_cast<size_t>(std::max(1.0, std::min(k_ideal, 30.0)));

    // Allocate the bit array — ceil(m_ / 8) bytes, zero-initialized.
    bits_.assign((m_ + 7) / 8, 0u);
}

BloomFilter::BloomFilter(size_t k, size_t m, std::vector<uint8_t> bits)
    : k_(k), m_(m), bits_(std::move(bits))
{}

// ============================================================================
// Base Hashes — FNV-1a + Murmur-Inspired Mix
// ============================================================================
//
// Two independent 64-bit hashes derived from the same key:
//
//   h1 — FNV-1a (Fowler–Noll–Vo): fast, byte-by-byte hashing with excellent
//         avalanche properties for short string keys.
//
//   h2 — A complementary Murmur-inspired finalizer applied to the FNV state,
//         giving a second hash that is statistically independent of h1.
//
// Together they drive the double hashing formula:
//   bit_i = (h1 + i * h2) mod m     for i in [0, k)
//
// Reference: Kirsch & Mitzenmacher, "Less Hashing, Same Performance:
//            Building a Better Bloom Filter" (ESA 2006)
// ============================================================================

std::pair<uint64_t, uint64_t> BloomFilter::base_hashes(const Key& key) {
    // FNV-1a for h1
    constexpr uint64_t kFNVPrime  = 0x00000100000001B3ULL;
    constexpr uint64_t kFNVOffset = 0xCBF29CE484222325ULL;

    uint64_t h1 = kFNVOffset;
    for (char ch : key) {
        h1 ^= static_cast<uint64_t>(static_cast<unsigned char>(ch));
        h1 *= kFNVPrime;
    }

    // Murmur finalizer mix for h2 — breaks linear correlations in h1.
    uint64_t h2 = h1;
    h2 ^= h2 >> 33;
    h2 *= 0xFF51AFD7ED558CCDULL;
    h2 ^= h2 >> 33;
    h2 *= 0xC4CEB9FE1A85EC53ULL;
    h2 ^= h2 >> 33;

    return { h1, h2 };
}

// ============================================================================
// Bit Array Accessors
// ============================================================================

void BloomFilter::set_bit(size_t bit_index) {
    bits_[bit_index / 8] |= static_cast<uint8_t>(1u << (bit_index % 8));
}

bool BloomFilter::get_bit(size_t bit_index) const {
    return (bits_[bit_index / 8] & (1u << (bit_index % 8))) != 0;
}

// ============================================================================
// add() — Set k Bits for a Key
// ============================================================================
//
// For each i in [0, k):
//   bit_position = (h1 + i * h2) mod m
//   set that bit
//
// The +i term ensures each hash function addresses a different position.
// Using (h2 | 1) guarantees h2 is odd, so gcd(h2, 2^64) = 1 — ensuring
// the sequence (h1 + i*h2) mod m visits distinct positions.
// ============================================================================

void BloomFilter::add(const Key& key) {
    auto [h1, h2] = base_hashes(key);
    h2 |= 1; // Ensure h2 is odd (coprime with any power-of-2 m for full coverage)

    for (size_t i = 0; i < k_; ++i) {
        const size_t bit_pos = (h1 + i * h2) % m_;
        set_bit(bit_pos);
    }
}

// ============================================================================
// might_contain() — Test k Bits for a Key
// ============================================================================

bool BloomFilter::might_contain(const Key& key) const {
    auto [h1, h2] = base_hashes(key);
    h2 |= 1;

    for (size_t i = 0; i < k_; ++i) {
        const size_t bit_pos = (h1 + i * h2) % m_;
        if (!get_bit(bit_pos)) {
            return false; // Definite miss — this key was never add()ed
        }
    }
    return true; // All bits set — probably present (may be a false positive)
}

// ============================================================================
// Serialization
// ============================================================================
//
// Format (all integers are little-endian uint32):
//
//   Offset   Size   Field    Description
//   ──────   ────   ─────    ────────────────────────────────────
//   0        4      k        Number of hash functions
//   4        4      m        Total bit count
//   8        N      bits     Packed bit array (N = ceil(m / 8) bytes)
//   ──────   ────   ─────    ────────────────────────────────────
//   Total: 8 + ceil(m / 8) bytes
//
// ============================================================================

std::vector<uint8_t> BloomFilter::serialize() const {
    std::vector<uint8_t> out;
    out.reserve(8 + bits_.size());

    // Helper: write a uint32 in little-endian byte order
    auto write_u32 = [&](uint32_t v) {
        out.push_back(static_cast<uint8_t>( v        & 0xFFu));
        out.push_back(static_cast<uint8_t>((v >>  8) & 0xFFu));
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
        out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
    };

    write_u32(static_cast<uint32_t>(k_));
    write_u32(static_cast<uint32_t>(m_));
    out.insert(out.end(), bits_.begin(), bits_.end());

    return out;
}

BloomFilter BloomFilter::deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < 8) {
        throw std::runtime_error(
            "BloomFilter::deserialize: buffer too small (< 8 bytes)");
    }

    auto read_u32 = [&](size_t offset) -> uint32_t {
        return  static_cast<uint32_t>(data[offset])
             | (static_cast<uint32_t>(data[offset + 1]) <<  8)
             | (static_cast<uint32_t>(data[offset + 2]) << 16)
             | (static_cast<uint32_t>(data[offset + 3]) << 24);
    };

    const size_t k = read_u32(0);
    const size_t m = read_u32(4);
    const size_t expected_bytes = (m + 7) / 8;

    if (data.size() < 8 + expected_bytes) {
        throw std::runtime_error(
            "BloomFilter::deserialize: truncated bit array");
    }

    std::vector<uint8_t> bits(data.begin() + 8,
                               data.begin() + 8 + static_cast<ptrdiff_t>(expected_bytes));
    return BloomFilter(k, m, std::move(bits));
}

} // namespace kvault
