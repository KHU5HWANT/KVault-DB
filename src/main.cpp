#include "kvault/api_routes.hpp"
#include "kvault/config.hpp"
#include "kvault/kvstore.hpp"

#include <iostream>
#include <memory>

int main() {
    std::cout << "Starting KVault Engine...\n";

    try {
        kvault::EngineConfig config;
        
        // You can customize config here before passing it to KVStore
        config.wal_directory = "data/wal";
        config.sstable_directory = "data/sstables";
        config.server_port = 8080;

        auto store = std::make_shared<kvault::KVStore>(config);
        
        std::cout << "KVault initialized.\n";
        std::cout << "Recovered WAL size: " << store->wal_size() << " bytes\n";
        std::cout << "Active SSTables: " << store->sstable_count() << "\n";

        kvault::ApiServer server(store, config.server_port);
        
        std::cout << "Starting HTTP API on port " << config.server_port << "...\n";
        std::cout << "Press Ctrl+C to stop.\n";
        
        server.run();

    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
