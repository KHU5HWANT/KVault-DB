#include "kvault/sstable_manager.hpp"

#include <algorithm>
#include <iostream>

namespace kvault {

SSTableManager::SSTableManager(const std::filesystem::path& sstable_dir)
    : sstable_dir_(sstable_dir)
{
    std::filesystem::create_directories(sstable_dir_);

    std::vector<std::filesystem::path> sst_files;
    for (const auto& entry : std::filesystem::directory_iterator(sstable_dir_)) {
        if (entry.is_regular_file() && entry.path().extension() == ".sst") {
            sst_files.push_back(entry.path());
        }
    }

    // Sort files in descending order (lexicographically by filename).
    // Assuming filenames are zero-padded timestamps like 1691234567.sst,
    // this correctly sorts newest first.
    std::sort(sst_files.begin(), sst_files.end(), std::greater<>());

    for (const auto& path : sst_files) {
        try {
            readers_.push_back(std::make_unique<SSTableReader>(path));
        } catch (const std::exception& e) {
            std::cerr << "SSTableManager: Failed to load " << path 
                      << " - " << e.what() << "\n";
        }
    }
}

std::optional<Value> SSTableManager::get(const Key& key) const {
    auto rec = get_record(key);
    if (!rec) return std::nullopt;
    if (rec->type == RecordType::DELETE) return std::string{};
    return rec->value;
}

std::optional<KVRecord> SSTableManager::get_record(const Key& key) const {
    // Search newest to oldest.
    for (const auto& reader : readers_) {
        auto result = reader->get_record(key);
        if (result) {
            // Found it! It could be a PUT or a DELETE (tombstone).
            // In either case, it's the most recent state for this key.
            return result;
        }
    }
    return std::nullopt;
}

void SSTableManager::add_sstable(const std::filesystem::path& path) {
    // A newly flushed SSTable is always the newest generation.
    auto reader = std::make_unique<SSTableReader>(path);
    readers_.insert(readers_.begin(), std::move(reader));
}

std::vector<SSTableMetadata> SSTableManager::get_metadata() const {
    std::vector<SSTableMetadata> meta;
    meta.reserve(readers_.size());
    for (const auto& r : readers_) {
        meta.push_back(r->metadata());
    }
    return meta;
}

} // namespace kvault
