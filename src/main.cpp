#include "kvault/api_routes.hpp"
#include "kvault/config.hpp"
#include "kvault/kvstore.hpp"

#include <iostream>
#include <memory>

int main(int argc, char* argv[]) {
    std::cout << "Starting KVault Engine...\n";

    try {
        kvault::EngineConfig config;
        
        std::string base_dir = "data"; // Portable default
        
        // Parse command line arguments
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--data-dir" && i + 1 < argc) {
                base_dir = argv[++i];
            }
        }
        
        // You can customize config here before passing it to KVStore
        config.wal_directory = base_dir + "/wal";
        config.sstable_directory = base_dir + "/sstables";
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
