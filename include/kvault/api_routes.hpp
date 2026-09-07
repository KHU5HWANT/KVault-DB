#pragma once

#include "kvault/kvstore.hpp"
#include <crow.h>
#include <crow/middlewares/cors.h>
#include <memory>

namespace kvault {

struct FixEmptyResponseMiddleware {
    struct context {};
    void before_handle(crow::request& /*req*/, crow::response& /*res*/, context& /*ctx*/) {}
    void after_handle(crow::request& req, crow::response& res, context& /*ctx*/) {
        // Render/Cloudflare strictly drops 204 No Content or empty 404s from Crow, throwing 502 Bad Gateway.
        if (req.method == crow::HTTPMethod::OPTIONS) {
            res.code = 200;
            if (res.body.empty()) res.body = "OK";
        }
        if (res.code == 404 && res.body.empty()) {
            res.body = "Not Found";
        }
    }
};

class ApiServer {
public:
    explicit ApiServer(std::shared_ptr<KVStore> store, uint16_t port);
    
    // Starts the HTTP server (blocks until interrupted)
    void run();
    
    // Stops the HTTP server
    void stop();

private:
    void setup_routes();

    std::shared_ptr<KVStore> store_;
    uint16_t port_;
    
    // Using CORS handler middleware
    crow::App<crow::CORSHandler, FixEmptyResponseMiddleware> app_;
};

} // namespace kvault
