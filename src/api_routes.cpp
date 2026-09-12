#include "kvault/api_routes.hpp"

namespace kvault {

ApiServer::ApiServer(std::shared_ptr<KVStore> store, uint16_t port)
    : store_(std::move(store)), port_(port)
{
    app_.loglevel(crow::LogLevel::Debug);

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
    
    // OPTIONS requests and 404s are natively handled by Crow, but modified by FixEmptyResponseMiddleware
    // to include a valid 200 OK or 404 body so Cloudflare doesn't throw 502 Bad Gateway.

    // GET /api/kv/<key>
    CROW_ROUTE(app_, "/api/kv/<string>").methods(crow::HTTPMethod::GET, crow::HTTPMethod::OPTIONS)(
        [this](const crow::request& req, crow::response& res, const std::string& key) {
            if (req.method == crow::HTTPMethod::OPTIONS) {
                res.code = 200;
                res.end();
                return;
            }
            auto val = store_->get(key);
            if (!val) {
                res.code = 404;
                res.body = "Not Found";
                res.end();
                return;
            }
            res.code = 200;
            res.body = *val;
            res.end();
        });

    // POST /api/kv
    // Expects JSON: { "key": "foo", "value": "bar" }
    CROW_ROUTE(app_, "/api/kv").methods(crow::HTTPMethod::POST, crow::HTTPMethod::OPTIONS)(
        [this](const crow::request& req, crow::response& res) {
            if (req.method == crow::HTTPMethod::OPTIONS) {
                res.code = 200;
                res.end();
                return;
            }
            auto x = crow::json::load(req.body);
            if (!x) {
                res.code = 400;
                res.body = "Invalid JSON";
                res.end();
                return;
            }
            if (!x.has("key") || !x.has("value")) {
                res.code = 400;
                res.body = "Missing 'key' or 'value'";
                res.end();
                return;
            }
            
            std::string key = x["key"].s();
            std::string value = x["value"].s();
            
            store_->put(key, value);
            res.code = 200;
            res.body = "OK";
            res.end();
        });

    // DELETE /api/kv/<key>
#ifdef DELETE
#undef DELETE
#endif
    CROW_ROUTE(app_, "/api/kv/<string>").methods(crow::HTTPMethod::DELETE)(
        [this](const std::string& key) {
            if (store_->remove(key)) {
                return crow::response(200, "OK");
            }
            return crow::response(404, "Key not found");
        });

    // GET /api/metrics
    CROW_ROUTE(app_, "/api/metrics").methods(crow::HTTPMethod::GET, crow::HTTPMethod::OPTIONS)(
        [this](const crow::request& req, crow::response& res) {
            if (req.method == crow::HTTPMethod::OPTIONS) {
                res.code = 200;
                res.end();
                return;
            }
            crow::json::wvalue x;
            
            x["memtable_size_bytes"] = store_->memtable_size();
            x["wal_size_bytes"] = store_->wal_size();
            x["sstable_count"] = store_->sstable_count();
            
            res.code = 200;
            res.body = x.dump();
            res.end();
        });

    // GET /api/memtable/snapshot
    // Returns all entries in the active MemTable as a JSON array.
    // Used by the React dashboard's SkipList visualizer.
    CROW_ROUTE(app_, "/api/memtable/snapshot").methods(crow::HTTPMethod::GET, crow::HTTPMethod::OPTIONS)(
        [this](const crow::request& req, crow::response& res) {
            if (req.method == crow::HTTPMethod::OPTIONS) {
                res.code = 200;
                res.end();
                return;
            }
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
            
            res.code = 200;
            res.body = result.dump();
            res.end();
        });
}

} // namespace kvault
