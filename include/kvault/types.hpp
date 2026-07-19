#pragma once

// ============================================================================
// types.hpp — Shared type definitions for the KVault storage engine
// ============================================================================
//
// This header defines the fundamental types used across all layers:
//   WAL → MemTable → SSTable → KVStore → API
//
// Centralizing these prevents circular dependencies and ensures a
// consistent vocabulary throughout the codebase.
// ============================================================================

#include <cstdint>
#include <string>
#include <string_view>

namespace kvault {

// ---------------------------------------------------------------------------
// Key & Value — the atomic units of storage
// ---------------------------------------------------------------------------
// Both are variable-length strings. Keys are compared lexicographically
// for ordered storage in the Skip List and SSTables.
// ---------------------------------------------------------------------------
using Key   = std::string;
using Value = std::string;

// ---------------------------------------------------------------------------
// RecordType — classifies every mutation in the system
// ---------------------------------------------------------------------------
// Stored as a single byte in the WAL binary format and SSTable records.
// Using uint8_t as the underlying type ensures exact 1-byte serialization.
// ---------------------------------------------------------------------------
enum class RecordType : uint8_t {
    PUT    = 0x00,  // Insert or overwrite a key-value pair
    DELETE = 0x01,  // Remove a key (written as a tombstone in the MemTable)
};

// ---------------------------------------------------------------------------
// KVRecord — the universal mutation unit
// ---------------------------------------------------------------------------
// Every write operation (PUT or DELETE) flowing through the system is
// represented as a KVRecord:
//
//   User PUT("x","42") → WAL::append({PUT,"x","42"}) → MemTable::put("x","42")
//   User DELETE("x")   → WAL::append({DELETE,"x",""}) → MemTable::remove("x")
//
// For DELETE records, the `value` field is empty — the deletion is
// represented by the RecordType alone. Inside the MemTable, deletes
// are stored as tombstone sentinels (see kTombstoneValue below).
// ---------------------------------------------------------------------------
struct KVRecord {
    RecordType type;
    Key        key;
    Value      value;   // Empty for DELETE records
};

// ---------------------------------------------------------------------------
// Tombstone Sentinel
// ---------------------------------------------------------------------------
// Used internally by the MemTable to mark deleted keys. The MemTable
// stores a tombstone by inserting the key with this sentinel as its value.
//
// This value is NEVER exposed to external callers:
//   - MemTable::get() returns std::nullopt when it encounters a tombstone
//   - MemTable::snapshot() converts tombstones to RecordType::DELETE records
//
// The sentinel is a non-printable byte sequence chosen to be virtually
// impossible to collide with real user data.
// ---------------------------------------------------------------------------
inline constexpr std::string_view kTombstoneValue =
    "\x7F__KVAULT_TOMBSTONE__\x7F";

} // namespace kvault
