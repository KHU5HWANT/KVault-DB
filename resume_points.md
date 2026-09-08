# KVault-DB | C++20, React, CMake, GoogleTest

- Built C++20 O(log n) Skip List MemTable via Arena Allocators, passing 10k-key test with zero stack overflows.
- Guaranteed crash durability & sub-100ms recovery via fsync Write-Ahead Log with constexpr CRC32.
- Optimized reads to 0.8% false positive rate via sparse SSTables & double-hashed Bloom Filters.
- Architected concurrent core using std::shared_mutex & O(1) tombstones, ensuring safety under 20 threads.
- Deployed containerized C++ API via Docker; built React telemetry dashboard with deterministic hashing.
