#include <crow.h>
#include <sstream>

#include "logging/logging.h"
#include "validation/script_cache.h"
#include "validation/parallel_config.h"
#include "api/routes.h"

int main() {
    // Initialize logging
    LOG_INFO("Starting Bitcoin Transaction Validator");
    
    // Set up kernel logging - this captures internal validation logs from bitcoinkernel
    setup_kernel_logging();
    
    // Log parallel verification configuration
    std::ostringstream parallel_info;
    parallel_info << "Parallel verification: " << (g_parallel_config.enabled ? "ENABLED" : "DISABLED")
                  << ", Threshold: " << ParallelVerificationConfig::MIN_INPUTS_FOR_PARALLEL << " inputs"
                  << ", Threads: " << g_parallel_config.get_thread_count();
    LOG_INFO(parallel_info.str());
    
    // Create HTTP server
    crow::SimpleApp app;
    
    // Register all routes
    register_all_routes(app);

    LOG_INFO("Starting HTTP server on port 8080");
    app.port(8080).multithreaded().run();
    
    // Print statistics before shutdown
    LOG_INFO("Server stopped, printing final statistics:");
    g_script_type_cache.print_stats();
    g_parallel_config.print_stats();
    
    // Clean up kernel logger before exit to prevent shutdown assertion failures
    LOG_INFO("Shutting down...");
    cleanup_kernel_logging();
    
    return 0;
}
