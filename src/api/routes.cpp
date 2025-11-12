#include "api/routes.h"
#include "validation/validator.h"
#include "validation/script_cache.h"
#include "validation/parallel_config.h"
#include "mempool/mempool.h"
#include "utils/hex_utils.h"
#include "utils/bitcoin_rpc.h"
#include "logging/logging.h"
#include "blockassembly/block_assembly.h"
#include <sstream>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <array>
#include <optional>
#include <algorithm>

/* -------------------- Hex & helpers -------------------- */
static std::string hex_encode(const std::vector<uint8_t>& v) {
    static const char* k = "0123456789abcdef";
    std::string s; s.reserve(v.size()*2);
    for (uint8_t b : v) { s.push_back(k[b>>4]); s.push_back(k[b&0xF]); }
    return s;
}
static std::vector<uint8_t> hex_decode(const std::string& h) {
    auto nyb = [](char c)->int{
        if (c>='0'&&c<='9') return c-'0';
        if (c>='a'&&c<='f') return c-'a'+10;
        if (c>='A'&&c<='F') return c-'A'+10;
        return -1;
    };
    if (h.size()%2) throw std::runtime_error("hex odd length");
    std::vector<uint8_t> out; out.reserve(h.size()/2);
    for (size_t i=0;i<h.size();i+=2) {
        int hi=nyb(h[i]), lo=nyb(h[i+1]);
        if (hi<0||lo<0) throw std::runtime_error("hex invalid");
        out.push_back(uint8_t((hi<<4)|lo));
    }
    return out;
}
// Print a 32-byte little-endian hash in user-facing big-endian hex (RPC style)
static std::string hex_rev(const std::array<uint8_t,32>& h_le) {
    static const char* k = "0123456789abcdef";
    std::string s; s.reserve(64);
    for (int i=31;i>=0;--i) { uint8_t b=h_le[i]; s.push_back(k[b>>4]); s.push_back(k[b&0xF]); }
    return s;
}
static std::array<uint8_t,32> hex256_le(const std::string& hex_be) {
    auto v = hex_decode(hex_be);
    if (v.size()!=32) throw std::runtime_error("hex256_le expects 32 bytes");
    std::array<uint8_t,32> a{}; for (int i=0;i<32;++i) a[i]=v[31-i]; return a;
}
static uint32_t parse_bits_be_to_uint32_t(const std::string& bits_hex_be) {
    auto v = hex_decode(bits_hex_be);
    if (v.size()!=4) throw std::runtime_error("bits must be 4 bytes");
    return (uint32_t(v[0])<<24)|(uint32_t(v[1])<<16)|(uint32_t(v[2])<<8)|uint32_t(v[3]); // write LE later
}

static bool contains_rule(const nlohmann::json& tpl, const std::string& rule) {
    if (!tpl.contains("rules")) return false;
    for (const auto& r : tpl["rules"]) if (r.get<std::string>()==rule) return true;
    return false;
}

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

    // POST /getblocktemplate - legacy RPC
    CROW_ROUTE(app, "/getblocktemplate").methods("POST"_method)([](const crow::request& req){
        auto j = crow::json::load(req.body);
        if (!j) {
            CROW_LOG_WARNING << "JSON parse failed. Body: " << req.body;
            return crow::response(400, "Invalid JSON (use plain ASCII quotes)");
        }
        if (!j.has("mode"))
        {
            return crow::response(400, "Missing getblocktemplate mode");
        }
        std::vector<uint8_t> payout_script = {0x51};
        /* TODO
        if (j.has("payout_script"))
        {

            payout_script = nlohmann::json(j["rules"]);
        }
        */
        try {
            std::string mode = j["mode"].s();
            nlohmann::json rules = nlohmann::json::array();

            if (j.has("rules")) {
                rules = nlohmann::json(j["rules"]);
            } else {
                rules.push_back(BlockTemplateRules::SEGWIT);
            }

            if (mode == BlockTemplateMode::PROPOSAL){
                if (!j.has("data")) {
                    return crow::response(400, std::string("Missing data for") + std::string(BlockTemplateMode::PROPOSAL) + std::string(" mode"));
                }
            } else {
                if (j.has("data")) {
                    return crow::response(400, std::string("Data must not be set for mode: ") + mode);
                }
            }
            std::string data = j.has("data") ? std::string(j["data"].s()) : "";

            BitcoinRPC rpc;

            // ask for a block template
            nlohmann::json result = rpc.get_blocktemplate(BlockTemplateMode::TEMPLATE, rules);

            // is a segwit block?
            bool segwit_active = contains_rule(result,"segwit") || result.contains("default_witness_commitment");

            // extract inputs fro block assembly
            std::vector<std::vector<uint8_t>> non_cb_txs;
            non_cb_txs.reserve(result["transactions"].size());

            for (const auto& t : result["transactions"]) non_cb_txs.push_back(hex_decode(t.at("data").get<std::string>()));
            std::optional<std::array<uint8_t,32>> txid0_le;
            if (!non_cb_txs.empty() && result["transactions"][0].contains("txid")) {
                txid0_le = hex256_le(result["transactions"][0]["txid"].get<std::string>());
            }

            blkasm::GbtBlockTemplateData in;
            in.version      = result.at("version").get<int32_t>();
            in.prev_hash_le = hex256_le(result.at("previousblockhash").get<std::string>());
            in.curtime      = result.at("curtime").get<uint32_t>();
            in.bits_u32     = parse_bits_be_to_uint32_t(result.at("bits").get<std::string>());
            in.coinbase_value = result.at("coinbasevalue").get<int64_t>();
            in.height       = result.at("height").get<int32_t>();
            if (result.contains("coinbaseaux") && result["coinbaseaux"].contains("flags")) {
                in.coinbase_flags = hex_decode(result["coinbaseaux"]["flags"].get<std::string>());
            }
            in.segwit_active = segwit_active;
            in.non_cb_tx_bytes = non_cb_txs;
            in.first_non_cb_expected_txid_le = txid0_le;

            // Assemble full block (no network/RPC dependencies here)
            auto block_proposal = blkasm::assemble_block_proposal(in, payout_script);

            // Propose the block
            auto blk_hex = hex_encode(block_proposal.block_bytes);
            nlohmann::json res = rpc.get_blocktemplate(BlockTemplateMode::PROPOSAL, rules, blk_hex);
            crow::response r(200);
            r.set_header("Content-Type", "application/json");
            if (res.is_null())
            {
                r.write("Valid Block");
            } else {
                r.write(res.dump());
            }
            // generate new block template with mempool

            // validate the new block template with getblocktemplate but with the proposal mode

            // return the new block template already validated

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
