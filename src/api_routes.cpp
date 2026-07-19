#include "kvault/api_routes.hpp"

namespace kvault {

ApiServer::ApiServer(std::shared_ptr<KVStore> store, uint16_t port)
    : store_(std::move(store)), port_(port)
{
    // Configure CORS
    auto& cors = app_.get_middleware<crow::CORSHandler>();
    cors.global()
        .headers("Content-Type")
        .methods("POST"_method, "GET"_method, "DELETE"_method, "OPTIONS"_method)
        .origin("*");

    setup_routes();
}

void ApiServer::run() {
    // Crow will run on the specified port. 
    // multithreaded() allows concurrent request handling.
    app_.port(port_).multithreaded().run();
}

void ApiServer::stop() {
    app_.stop();
}

void ApiServer::setup_routes() {
    
    // GET /api/kv/<key>
    CROW_ROUTE(app_, "/api/kv/<string>").methods(crow::HTTPMethod::GET)(
        [this](const std::string& key) {
            auto val = store_->get(key);
            if (!val) {
                return crow::response(404, "Key not found");
            }
            return crow::response(200, *val);
        });

    // POST /api/kv
    // Expects JSON: { "key": "foo", "value": "bar" }
    CROW_ROUTE(app_, "/api/kv").methods(crow::HTTPMethod::POST)(
        [this](const crow::request& req) {
            auto x = crow::json::load(req.body);
            if (!x) {
                return crow::response(400, "Invalid JSON");
            }
            if (!x.has("key") || !x.has("value")) {
                return crow::response(400, "Missing 'key' or 'value'");
            }
            
            std::string key = x["key"].s();
            std::string value = x["value"].s();
            
            store_->put(key, value);
            return crow::response(200, "OK");
        });

    // DELETE /api/kv/<key>
#ifdef DELETE
#undef DELETE
#endif
    CROW_ROUTE(app_, "/api/kv/<string>").methods(crow::HTTPMethod::DELETE)(
        [this](const std::string& key) {
            bool removed = store_->remove(key);
            if (!removed) {
                // Return 200 even if it wasn't there, delete is idempotent in LSM
                return crow::response(200, "OK (was not present)");
            }
            return crow::response(200, "OK");
        });

    // GET /api/metrics
    CROW_ROUTE(app_, "/api/metrics").methods(crow::HTTPMethod::GET)(
        [this]() {
            crow::json::wvalue metrics;
            metrics["memtable_size_bytes"] = store_->memtable_size();
            metrics["wal_size_bytes"] = store_->wal_size();
            metrics["sstable_count"] = store_->sstable_count();
            return crow::response(metrics);
        });

    // GET /api/memtable/snapshot
    // Returns all entries in the active MemTable as a JSON array.
    // Used by the React dashboard's SkipList visualizer.
    CROW_ROUTE(app_, "/api/memtable/snapshot").methods(crow::HTTPMethod::GET)(
        [this]() {
            auto records = store_->memtable_snapshot();
            crow::json::wvalue::list arr;
            arr.reserve(records.size());
            for (const auto& rec : records) {
                crow::json::wvalue entry;
                entry["key"]   = rec.key;
                entry["value"] = rec.value;
                entry["type"]  = (rec.type == RecordType::DELETE) ? "tombstone" : "put";
                arr.push_back(std::move(entry));
            }
            crow::json::wvalue result;
            result["entries"] = std::move(arr);
            result["count"]   = records.size();
            return crow::response(result);
        });
}

} // namespace kvault
