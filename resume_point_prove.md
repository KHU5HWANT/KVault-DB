# KVault-DB: Resume Bullet Points Proof & Defense Guide

This document contains the exact proof and procedures to defend every single bullet point on your updated resume during a technical interview.

## Point 1
> **"KVault LSM-Tree DB | [GitHub] | [Demo] — Engineered a persistent LSM-Tree C++20 key-value store from scratch."**

**How to prove it:**
1. **The Proof:** The entire GitHub repository and live Render dashboard prove this. The backend is written in modern C++20, and the `data/` folder persistently stores the SSTable files on disk.

---

## Point 2
> **"Preempted SkipList stack-overflow crashes during massive node deletions by building a flat C++20 Arena Allocator."**

**How to prove it:**
1. **The Procedure:** Open `src/arena.cpp` and `include/kvault/arena.hpp`.
2. **The Defense:** If an interviewer asks how you avoided stack-overflows during destruction, explain that destroying a massive linked data structure (like a 10,000 node SkipList) usually triggers recursive destructor calls that blow up the stack limit. You prevented this by building a custom **Arena Allocator**. You allocated chunks of flat memory sequentially and stored the nodes as non-owning raw pointers (`Node**`). When the MemTable is destroyed, you just free the massive flat memory chunks instantly in O(1) time without triggering any recursive destructors!

---

## Point 3
> **"Engineered 100% crash durability with sub-100ms recovery via WAL records and constexpr CRC32 lookup tables."**

**How to prove it:**
1. **The Procedure:** Open `include/kvault/wal.hpp` (Lines 132-134).
2. **The Defense:** If asked about crash recovery, point them to your Write-Ahead Log (WAL). Explain that every write is `fsync`'d to disk before acknowledging the client. If they ask about the `constexpr CRC32`, explain that calculating CRC32 requires a 256-entry lookup table. Instead of computing this table at runtime (which takes CPU cycles on startup), you used C++20's `constexpr` to force the compiler to calculate the table during compilation! The bytes are embedded directly into the binary, ensuring zero-cost initialization and sub-100ms recovery times on boot.

---

## Point 4
> **"Bypassed redundant disk seeks, hitting a ~0.8% false positive rate via sparse block index and coprime Bloom Filters."**

**How to prove it:**
1. **The Procedure:** Open `src/bloom_filter.cpp` (Line 99).
2. **The Defense:** This is a killer mathematical point. The Kirsch-Mitzenmacher double-hashing algorithm uses two hash functions: `hash1` and `hash2`. To probe the bit array, you calculate the index as `(hash1 + i * hash2) % m`. 
If they ask what "coprime" means here, point them to **Line 99**: `h2 |= 1; // Ensure h2 is odd`.
Because your bit array size (`m`) is a power of 2, forcing `hash2` to be an odd number guarantees that `hash2` and `m` are **coprime** (they share no common factors). This mathematical trick guarantees that your probe sequence will visit every single bit in the array without trapping itself in an infinite loop!

---

## Point 5
> **"Architected thread-safe operations, passing a 20-thread stress test of 5,000 keys with no shared_mutex deadlocks."**

**How to prove it:**
1. **The Procedure:** Run the Python stress testing script against your live server: `python stress_test.py`
2. **The Defense:** The python script literally spins up 20 parallel threads firing thousands of PUT, GET, and DELETE requests simultaneously. If they ask how you prevented deadlocks in the C++ code, open `include/kvault/kvstore.hpp` and point out the `std::shared_mutex rw_mutex_`. Explain the Reader-Writer lock pattern: multiple threads can read safely at the exact same time (using `std::shared_lock`), but write operations take an exclusive lock (`std::unique_lock`). Also mention that deletes are processed as O(1) "tombstones" (blind writes), which avoids the need to read-lock before write-locking, completely eliminating read-write deadlocks!
