# KVault-DB — ATS-Optimized Resume Bullet Points

---

- **Architected** a full-stack, single-node LSM-Tree key-value store in C++20, implementing every layer — arena-allocated probabilistic Skip List, binary WAL, SSTable serialization, and Bloom Filter — without third-party storage libraries; validated by **74/74 GoogleTest cases passing in 1.58 seconds** with zero warnings under `-Wall -Wextra -Wpedantic`.

- **Engineered** a concurrent request pipeline using `std::shared_mutex` Reader-Writer locks, enabling parallel `GET` operations via `shared_lock` and serialized `PUT`/`DELETE` writes via `unique_lock`; verified **zero memory corruption across 5,000 concurrent operations dispatched from 20 threads**, confirming 100% read consistency under stress.

- **Implemented** a crash-durable Write-Ahead Log with a `constexpr` CRC32-validated binary record format (13 + K + V bytes per record, reflected polynomial `0xEDB88320`), guaranteeing atomic WAL-first durability — every mutation is fsynced to stable storage before the MemTable is updated, with replay stopping at the exact corruption boundary on recovery.

- **Designed** a two-hash Bloom Filter (Kirsch–Mitzenmacher double-hashing, FNV-1a + Murmur3 mix, k=7 probes, ~0.8% false positive rate at 10 bits/key) to eliminate disk I/O on absent-key lookups, integrated with a sparse index and cascading newest-to-oldest SSTable search path across a 4 MB MemTable flush threshold.

- **Optimized** the SkipList memory model using an arena-allocation pattern (`std::vector<std::unique_ptr<Node>>` owning all nodes with non-owning raw `Node*` forward pointers), eliminating both double-free undefined behaviour and recursive destructor stack overflow at arbitrary scale, while preserving pointer stability across `vector` reallocations.
