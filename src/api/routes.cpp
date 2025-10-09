#include "api/routes.h"
#include "validation/validator.h"
#include "validation/script_cache.h"
#include "validation/parallel_config.h"
#include "mempool/mempool.h"
#include "utils/hex_utils.h"
#include "utils/bitcoin_rpc.h"
#include "logging/logging.h"
#include <sstream>

// Validation routes
void register_validation_routes(crow::SimpleApp& app) {
    // POST /verify with JSON: { "tx_hex": "...hex..." }
    CROW_ROUTE(app, "/verify").methods("POST"_method)([](const crow::request& req){
        auto body = crow::json::load(req.body);
        if (!body || !body.has("tx_hex")) {
            LOG_WARN("Received request without tx_hex");
            return crow::response(400, "Missing tx_hex");
        }

        try {
            std::string tx_hex = body["tx_hex"].s();
            LOG_INFO("Received validation request for transaction");

            // Call the validation function
            ValidationResult result = validate_transaction(tx_hex);

            // Build response
            crow::json::wvalue res;
            res["txid"] = result.txid;
            res["fee_sats"] = result.fee_sats;

            std::ostringstream success_msg;
            success_msg << "Validation successful for txid: " << result.txid;
            LOG_INFO(success_msg.str());
            
            return crow::response(200, res);

        } catch (const ValidationError& e) {
            std::ostringstream err_msg;
            err_msg << "Validation error: " << e.what();
            LOG_ERROR(err_msg.str());
            
            // Return validation error with status information
            crow::json::wvalue error_res;
            error_res["error"] = e.what();
            error_res["status"] = e.status;
            error_res["status_name"] = status_to_string(e.status);
            error_res["input_index"] = static_cast<int>(e.input_index);

            return crow::response(400, error_res);
        } catch (const std::exception& e) {
            std::ostringstream err_msg;
            err_msg << "Exception during validation: " << e.what();
            LOG_ERROR(err_msg.str());
            return crow::response(400, std::string("error: ") + e.what());
        }
    });
}

// Mempool routes
void register_mempool_routes(crow::SimpleApp& app) {
    // POST /mempool/add - Add transaction to private mempool
    CROW_ROUTE(app, "/mempool/add").methods("POST"_method)([](const crow::request& req){
        auto body = crow::json::load(req.body);
        if (!body || !body.has("tx_hex")) {
            return crow::response(400, "Missing tx_hex");
        }
        
        try {
            std::string tx_hex = body["tx_hex"].s();
            LOG_INFO("Received mempool add request");
            
            // Reuse validate_transaction to ensure transaction is valid
            ValidationResult result = validate_transaction(tx_hex);
            
            // Get transaction size for fee rate calculation
            std::vector<std::byte> tx_bytes = from_hex(tx_hex);
            size_t tx_size = tx_bytes.size();
            
            // Create mempool entry (automatically calculates fee rate)
            MempoolEntry entry(result.txid, tx_hex, result.fee_sats, tx_size);
            
            // Add to mempool (will be automatically ordered by fee rate)
            g_mempool.add(entry);
            
            // Build response
            crow::json::wvalue res;
            res["txid"] = entry.txid;
            res["fee_sats"] = entry.fee_sats;
            res["size_bytes"] = entry.tx_size;
            res["fee_rate"] = entry.fee_rate;
            res["message"] = "Transaction added to mempool";
            res["mempool_size"] = g_mempool.size();
            
            return crow::response(200, res);
            
        } catch (const ValidationError& e) {
            std::ostringstream err_msg;
            err_msg << "Validation error, transaction rejected: " << e.what();
            LOG_ERROR(err_msg.str());
            
            crow::json::wvalue error_res;
            error_res["error"] = e.what();
            error_res["status"] = e.status;
            error_res["status_name"] = status_to_string(e.status);
            error_res["input_index"] = static_cast<int>(e.input_index);
            
            return crow::response(400, error_res);
        } catch (const std::exception& e) {
            LOG_ERROR(std::string("Mempool add failed: ") + e.what());
            return crow::response(400, std::string("error: ") + e.what());
        }
    });

    // GET /mempool - List all transactions ordered by fee rate (highest first)
    CROW_ROUTE(app, "/mempool").methods("GET"_method)([](){
        auto entries = g_mempool.get_all();
        
        crow::json::wvalue res;
        res["count"] = entries.size();
        
        std::vector<crow::json::wvalue> txs;
        for (const auto& entry : entries) {
            crow::json::wvalue tx;
            tx["txid"] = entry.txid;
            tx["fee_sats"] = entry.fee_sats;
            tx["size_bytes"] = entry.tx_size;
            tx["fee_rate"] = entry.fee_rate;
            txs.push_back(std::move(tx));
        }
        res["transactions"] = std::move(txs);
        
        return crow::response(200, res);
    });

    // DELETE /mempool/<txid> - Remove transaction from mempool
    CROW_ROUTE(app, "/mempool/<string>").methods("DELETE"_method)([](const std::string& txid){
        bool removed = g_mempool.remove(txid);
        
        if (removed) {
            crow::json::wvalue res;
            res["message"] = "Transaction removed from mempool";
            res["txid"] = txid;
            res["mempool_size"] = g_mempool.size();
            return crow::response(200, res);
        } else {
            return crow::response(404, "Transaction not found in mempool");
        }
    });

    // POST /mempool/clear - Clear all transactions from mempool
    CROW_ROUTE(app, "/mempool/clear").methods("POST"_method)([](){
        g_mempool.clear();
        
        crow::json::wvalue res;
        res["message"] = "Mempool cleared";
        res["mempool_size"] = 0;
        
        return crow::response(200, res);
    });
}

// Statistics routes
void register_stats_routes(crow::SimpleApp& app) {
    // GET /cache-stats - returns script type cache statistics
    CROW_ROUTE(app, "/cache-stats").methods("GET"_method)([](){
        size_t hits = g_script_type_cache.get_hits();
        size_t misses = g_script_type_cache.get_misses();
        size_t total = hits + misses;
        double hit_rate = 0.0;
        if (total > 0) {
            hit_rate = (static_cast<double>(hits) / total) * 100.0;
        }
        
        crow::json::wvalue stats;
        stats["cache_hits"] = hits;
        stats["cache_misses"] = misses;
        stats["total_lookups"] = total;
        stats["hit_rate_percent"] = hit_rate;
        stats["cache_size"] = g_script_type_cache.get_size();
        stats["cache_max_size"] = ScriptTypeCache::MAX_CACHE_SIZE;
        
        return crow::response(200, stats);
    });

    // POST /cache-clear - clears the script type cache
    CROW_ROUTE(app, "/cache-clear").methods("POST"_method)([](){
        g_script_type_cache.clear();
        
        crow::json::wvalue res;
        res["message"] = "Script type cache cleared successfully";
        
        return crow::response(200, res);
    });

    // GET /parallel-stats - returns parallel verification statistics
    CROW_ROUTE(app, "/parallel-stats").methods("GET"_method)([](){
        size_t parallel_validations = g_parallel_config.get_parallel_validations();
        size_t sequential_validations = g_parallel_config.get_sequential_validations();
        size_t total_validations = parallel_validations + sequential_validations;
        double parallel_pct = 0.0;
        if (total_validations > 0) {
            parallel_pct = (static_cast<double>(parallel_validations) / total_validations) * 100.0;
        }
        
        crow::json::wvalue stats;
        stats["enabled"] = g_parallel_config.enabled;
        stats["min_inputs_threshold"] = ParallelVerificationConfig::MIN_INPUTS_FOR_PARALLEL;
        stats["max_threads"] = g_parallel_config.max_threads;
        stats["detected_threads"] = g_parallel_config.get_thread_count();
        stats["parallel_validations"] = parallel_validations;
        stats["sequential_validations"] = sequential_validations;
        stats["total_validations"] = total_validations;
        stats["parallel_percent"] = parallel_pct;
        stats["total_inputs_parallel"] = g_parallel_config.get_total_inputs_parallel();
        stats["total_inputs_sequential"] = g_parallel_config.get_total_inputs_sequential();
        
        return crow::response(200, stats);
    });

    // POST /parallel-config - configure parallel verification
    CROW_ROUTE(app, "/parallel-config").methods("POST"_method)([](const crow::request& req){
        auto body = crow::json::load(req.body);
        if (!body) {
            return crow::response(400, "Invalid JSON");
        }
        
        if (body.has("enabled")) {
            g_parallel_config.enabled = body["enabled"].b();
        }
        
        if (body.has("max_threads")) {
            int threads = body["max_threads"].i();
            if (threads < 0 || threads > 64) {
                return crow::response(400, "max_threads must be between 0 and 64");
            }
            g_parallel_config.max_threads = static_cast<size_t>(threads);
        }
        
        crow::json::wvalue res;
        res["message"] = "Parallel verification configuration updated";
        res["enabled"] = g_parallel_config.enabled;
        res["max_threads"] = g_parallel_config.max_threads;
        res["detected_threads"] = g_parallel_config.get_thread_count();
        
        LOG_INFO("Parallel verification config updated");
        
        return crow::response(200, res);
    });
}

// Legacy routes (for backward compatibility)
void register_legacy_routes(crow::SimpleApp& app) {
    // POST /gettxout - legacy RPC passthrough
    CROW_ROUTE(app, "/gettxout").methods("POST"_method)([](const crow::request& req){
        auto j = crow::json::load(req.body);
        if (!j) {
            CROW_LOG_WARNING << "JSON parse failed. Body: " << req.body;
            return crow::response(400, "Invalid JSON (use plain ASCII quotes)");
        }
        if (!j.has("txid") || !j.has("vout")) {
            return crow::response(400, "Missing txid/vout");
        }
        try {
            std::string txid = j["txid"].s();
            int vout = j["vout"].i();
            bool include_mempool = j.has("include_mempool") ? j["include_mempool"].b() : true;

            BitcoinRPC rpc;
            nlohmann::json result = rpc.get_txout(txid, vout, include_mempool);

            crow::response r(200);
            r.set_header("Content-Type", "application/json");
            r.write(result.dump());
            return r;
        } catch (const std::exception& e) {
            return crow::response(500, std::string("RPC failed: ") + e.what());
        }
    });
}

// Register all routes
void register_all_routes(crow::SimpleApp& app) {
    register_validation_routes(app);
    register_mempool_routes(app);
    register_stats_routes(app);
    register_legacy_routes(app);
}
