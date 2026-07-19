#pragma once

#include "kvault/kvstore.hpp"
#include <crow.h>
#include <crow/middlewares/cors.h>
#include <memory>

namespace kvault {

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
    crow::App<crow::CORSHandler> app_;
};

} // namespace kvault
