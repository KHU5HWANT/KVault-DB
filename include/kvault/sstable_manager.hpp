#pragma once

#include "kvault/sstable_reader.hpp"
#include "kvault/types.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace kvault {

class SSTableManager {
public:
    // Initialize the manager, scanning the given directory for .sst files.
    // Loads them ordered from newest to oldest based on filename.
    explicit SSTableManager(const std::filesystem::path& sstable_dir);

    // Look up a key across all loaded SSTables, starting from the newest.
    // Returns:
    //   std::optional<Value> containing the value
    //   std::optional<Value> containing "" if it's a DELETE tombstone
    //   std::nullopt if the key is not found in any SSTable
    [[nodiscard]] std::optional<Value> get(const Key& key) const;
    [[nodiscard]] std::optional<KVRecord> get_record(const Key& key) const;

    // Register a newly flushed SSTable. It is pushed to the front
    // (most recent) of the reader list.
    void add_sstable(const std::filesystem::path& path);

    // Get aggregated metadata for all loaded SSTables.
    [[nodiscard]] std::vector<SSTableMetadata> get_metadata() const;

    // The number of active SSTables being managed.
    [[nodiscard]] size_t count() const noexcept { return readers_.size(); }

private:
    std::filesystem::path sstable_dir_;
    
    // Ordered from newest (index 0) to oldest (index N-1).
    std::vector<std::unique_ptr<SSTableReader>> readers_;
};

} // namespace kvault
