# KVault-DB — Final Resume Bullet Points

- Implemented $O(\log n)$ write buffering as measured by zero recursive stack overflows over a 10,000-key stress test, by building a 16-level probabilistic Skip List MemTable in C++20 backed by an Arena Allocator and non-owning raw traversal pointers.

- Engineered 100% crash durability with sub-100 ms database recovery by designing a binary Write-Ahead Log (WAL) using a compile-time (constexpr) 256-entry CRC32 lookup table and platform-specific fsync barriers to detect and isolate partially written records.

- Optimized disk read latency and eliminated redundant I/O seeks, achieving a ~0.8% false positive rate on absent key queries, by writing a sequential SSTable persistence layer featuring a 100-key interval sparse index and an embedded Bloom Filter driven by Kirsch–Mitzenmacher double-hashing.

- Architected a highly concurrent storage core, verifying 100% read consistency under a 20-thread stress test of 5,000 requests, by coordinating a std::shared_mutex Reader-Writer lock and executing deletes as blind writes with a dedicated tombstone sentinel to prevent thread deadlocks.

- Developed a multi-threaded REST API using Crow and a React/Vite visualization dashboard to expose real-time engine telemetry, achieving a 2-second metrics polling latency by decoupling client-side Skip List rendering from backend mutex locks via deterministic key-based hash seeding.
