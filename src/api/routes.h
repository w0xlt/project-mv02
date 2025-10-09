#pragma once

#include <crow.h>

// Register all HTTP routes
void register_all_routes(crow::SimpleApp& app);

// Individual route registration functions
void register_validation_routes(crow::SimpleApp& app);
void register_mempool_routes(crow::SimpleApp& app);
void register_stats_routes(crow::SimpleApp& app);
void register_legacy_routes(crow::SimpleApp& app);
