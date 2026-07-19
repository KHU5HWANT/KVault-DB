#include "kvault/kvstore.hpp"
#include "kvault/sstable_writer.hpp"

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>

namespace kvault {

namespace {
// Generates a zero-padded timestamp filename like "000001691234567.sst"
// to ensure lexicographical sorting matches chronological order.
std::string generate_sstable_filename() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(16) << ms << ".sst";
    return oss.str();
}
} // namespace

KVStore::KVStore(const EngineConfig& config)
    : config_(config)
{
    // Ensure directories exist
    std::filesystem::create_directories(config_.wal_directory);
    std::filesystem::create_directories(config_.sstable_directory);

    // Initialize subsystems
    sstable_manager_ = std::make_unique<SSTableManager>(config_.sstable_directory);
    
    std::filesystem::path wal_dir = std::filesystem::path(config_.wal_directory);
    wal_ = std::make_unique<WriteAheadLog>(wal_dir, config_.sync_per_write);
    
    memtable_ = std::make_unique<MemTable>(
        config_.memtable_flush_threshold_bytes
    );

    // Replay WAL into MemTable if the WAL file already has data
    auto recovered_records = wal_->replay();
    for (const auto& rec : recovered_records) {
        if (rec.type == RecordType::PUT) {
            memtable_->put(rec.key, rec.value);
        } else if (rec.type == RecordType::DELETE) {
            memtable_->remove(rec.key);
        }
    }
}

void KVStore::put(const Key& key, const Value& value) {
    std::unique_lock lock(rw_mutex_);

    wal_->append(KVRecord{RecordType::PUT, key, value});
    memtable_->put(key, value);

    if (memtable_->should_flush()) {
        trigger_flush();
    }
}

std::optional<Value> KVStore::get(const Key& key) const {
    std::shared_lock lock(rw_mutex_);

    // 1. Check MemTable
    if (memtable_->contains_tombstone(key)) {
        return std::nullopt; // Key was deleted recently
    }

    auto mem_val = memtable_->get(key);
    if (mem_val) {
        return mem_val;
    }

    // 2. Fallback to SSTableManager
    auto sst_val = sstable_manager_->get(key);
    if (sst_val && sst_val->empty()) {
        // Returned empty string from SSTableManager means DELETE tombstone
        return std::nullopt;
    }
    return sst_val;
}

bool KVStore::remove(const Key& key) {
    std::unique_lock lock(rw_mutex_);
    
    // We append a tombstone regardless of whether the key exists,
    // as it's an append-only operation that supersedes any older PUTs.
    wal_->append(KVRecord{RecordType::DELETE, key, ""});
    
    // In an LSM tree, deletes are blind writes. Returning true unconditionally
    // avoids a costly read and prevents a deadlock (cannot acquire shared_lock
    // while holding unique_lock).
    memtable_->remove(key);

    if (memtable_->should_flush()) {
        trigger_flush();
    }
    
    return true;
}

void KVStore::force_flush() {
    std::unique_lock lock(rw_mutex_);
    if (memtable_->current_size_bytes() > 0) {
        trigger_flush();
    }
}

void KVStore::trigger_flush() {
    // Note: Calling method must hold unique_lock on rw_mutex_
    auto records = memtable_->snapshot();
    if (records.empty()) return;

    std::filesystem::path sst_path = std::filesystem::path(config_.sstable_directory) / generate_sstable_filename();
    
    SSTableWriter::write(sst_path, records, config_.bloom_filter_bits_per_key);
    
    sstable_manager_->add_sstable(sst_path);
    
    // Clear MemTable (creates a new instance)
    memtable_ = std::make_unique<MemTable>(
        config_.memtable_flush_threshold_bytes
    );
    
    // Clear WAL (truncate file)
    wal_->truncate();
}

size_t KVStore::memtable_size() const {
    std::shared_lock lock(rw_mutex_);
    return memtable_->current_size_bytes();
}

size_t KVStore::sstable_count() const {
    std::shared_lock lock(rw_mutex_);
    return sstable_manager_->count();
}

size_t KVStore::wal_size() const {
    std::shared_lock lock(rw_mutex_);
    return wal_->file_size();
}

std::vector<KVRecord> KVStore::memtable_snapshot() const {
    std::shared_lock lock(rw_mutex_);
    return memtable_->snapshot();
}

} // namespace kvault
