#include "api/routes.h"
#include "validation/validator.h"
#include "validation/script_cache.h"
#include "validation/parallel_config.h"
#include "mempool/mempool.h"
#include "utils/hex_utils.h"
#include "logging/logging.h"
#include "blockassembly/block_assembly.h"

#include <sstream>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <unordered_set>

/* -------------------- Helpers -------------------- */
static bool contains_rule(const nlohmann::json& tpl, const std::string& rule) {
    if (!tpl.contains("rules")) return false;
    for (const auto& r : tpl["rules"]) if (r.get<std::string>()==rule) return true;
    return false;
}

static void check_if_unconfirmed_parent(std::string parent_tx_id, std::vector<btck::Transaction>& tx_chain, std::unordered_set<std::string>& visited) {

    LOG_INFO("Checking if " + parent_tx_id + " is an unconfirmed parent");
    if (visited.contains(parent_tx_id)) 
    {
        LOG_INFO(parent_tx_id + " was already visited in the past, not revisitng");
        return;
    }
    visited.insert(parent_tx_id);

    BitcoinRPC rpc;
    try {
        nlohmann::json result = rpc.get_rawtransaction(parent_tx_id);
        if (result.contains("error")) return; // parent is already confirmed
        else if (result.contains("vin") && result["vin"].is_array())
        {
            btck::Transaction parent_tx(from_hex(result["hex"]));
            for (const auto& input: result["vin"])
            {
                LOG_INFO(parent_tx_id + " is an unconfirmed parent");
                check_if_unconfirmed_parent(input["txid"], tx_chain, visited);
            }
            tx_chain.insert(tx_chain.begin(), parent_tx);
        }
    }
    catch (const std::runtime_error& e){
        std::ostringstream err_msg;
        err_msg << "Could not retrieve parent from the mempool. Assuming " + parent_tx_id + " is already confirmed): " << e.what();
        LOG_ERROR(err_msg.str());
        // RPC call returned an erro, maybe parent already confirmed
        // Return as if it was confirmed
        return;
    }
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

            LOG_INFO("Block template request received");
            // Vector of hex transactions in bytes to be added to the block
            std::vector<std::vector<uint8_t>> non_cb_txs;
            // fees collected in the block in sats
            int64_t collected_fees = 0;

            // Collect all private txs and their unconfirmed ancestors
            std::vector<btck::Transaction> tx_chain;
            std::unordered_set<std::string> parents_visited;

            // Vector of our private transactions
            std::vector<MempoolEntry> private_mempool_entries = g_mempool.get_all();
            LOG_INFO("Actual private mempool size: " + std::to_string(private_mempool_entries.size()));
            for (const MempoolEntry& private_tx : private_mempool_entries)
            {
                btck::Transaction tx(from_hex(private_tx.tx_hex));

                // NOTE: OUR PRIVATE MEMPOOL SHOULD BE ORDERED BY ANCESTOR ALSO TO MAKE THIS WORK
                // IF NOT WE COULD MISS CPFP FROM THE PRIVATE MEMPOOL
                // NOTE: OUR PRIVATE MEMPOOL HAS TO CHECK THAT THERE ARE NO DOUBLE SPENDING INSIDE THE
                // PRIVATE MEMPOOL, IF THERE ARE WE CAN TAKE THE ONE WITH HIGHER FEERATE

                // Check that it does not have unconfirmed parents in the mempool if it does pick them too
                for (const btck::TransactionInput& input_view : tx.Inputs())
                {
                    const std::string parent_tx_id = txid_to_hex_reversed(input_view.OutPoint().Txid().ToBytes());
                    check_if_unconfirmed_parent(parent_tx_id, tx_chain, parents_visited);
                }
                tx_chain.push_back(tx);
                collected_fees += private_tx.fee_sats;
            }
            LOG_INFO("Private mempool transactions and parents considered. Size of the tx chain is " + std::to_string(tx_chain.size()));

            // Add the private txs and unconfirmed ancestors to our ToBe-mined transaction list.
            // Also collect all OutPoints the transactions are spending to prevent double spending
            // in block template transactions
            std::vector<btck::OutPoint> already_spent_outpoints;
            for (const btck::Transaction& tx : tx_chain)
            {
                // Convert tx bytes to correct unit format to build the block template
                std::vector<unsigned char> v;
                for(std::byte b : tx.ToBytes())
                {
                    v.push_back(static_cast<uint8_t>(b));
                }
                non_cb_txs.push_back(std::move(v));

                // Add outpoints to the already spent list to check in the future
                // for possible double spendings
                for (const btck::TransactionInput& input_view : tx.Inputs())
                {
                    already_spent_outpoints.push_back(btck::OutPoint(input_view.OutPoint()));
                }
            }
            LOG_INFO("Private mempool transactions and parents added to the block template transactions.\n"
                "Chain of already spent outputs size is " + std::to_string(already_spent_outpoints.size()));

            // ask for a block template
            BitcoinRPC rpc;
            nlohmann::json result = rpc.get_blocktemplate(BlockTemplateMode::TEMPLATE, rules);
            
            // Add getblocktemplate transactions to our ToBe-mined transaction list
            // Additional check if they cause a doble spending before adding
            // If a transaction is removed because double spending add it to
            // removed list because dependents must also be removed

            int32_t good_tx_candidates_count = 0;
            std::unordered_set<std::string> removed_transactions_ids;
            for (const auto& tx_res : result["transactions"])
            {
                std::string tx_hex = tx_res.at("data").get<std::string>();
                std::vector<std::byte> tx_bytes = from_hex(tx_hex);
                btck::Transaction tx(tx_bytes);
                std::string tx_id = txid_to_hex_reversed(tx.Txid().ToBytes());

                bool should_remove = false;
                for(const btck::TransactionInput& input_view : tx.Inputs())
                {
                    // Should remove if the parent was also removed
                    if (removed_transactions_ids.contains(txid_to_hex_reversed(input_view.OutPoint().Txid().ToBytes())))
                    {
                        LOG_INFO(tx_id + " will be removed because his parents were also removed");
                        should_remove = true;
                        break;
                    }
                    // Should remove if the transaction has a double spending colision with
                    // some transaction from the private mempool
                    for (btck::OutPoint& priv_tx_outpoint : already_spent_outpoints)
                    {
                        if (priv_tx_outpoint.Txid() == input_view.OutPoint().Txid()
                            && priv_tx_outpoint.index() == input_view.OutPoint().index())
                        {
                            if (parents_visited.contains(tx_id)) LOG_INFO(tx_id + " will be removed because it was already added for being an unconfirmed parent of a secret transaction");
                            else LOG_INFO(tx_id + " will be removed because causes it double spends " +txid_to_hex_reversed(priv_tx_outpoint.Txid().ToBytes()));
                            should_remove = true;
                            break;
                        }
                    }
                    if (should_remove) break;
                }

                // If not double spend add it to the TO-BE mined transactions
                if(should_remove)
                {
                    removed_transactions_ids.insert(tx_id);
                    continue;
                }

                // Convert tx bytes to correct unit format to build the block template
                std::vector<unsigned char> v;
                for(std::byte b : tx.ToBytes())
                {
                    v.push_back(static_cast<uint8_t>(b));
                }
                non_cb_txs.push_back(std::move(v));
                good_tx_candidates_count++;

                // Add the transaction absolute fee to the total collected fees
                int64_t tx_fee = tx_res.at("fee").get<int64_t>();
                collected_fees += tx_fee;
            }
            LOG_INFO("Public mempool transactions processed: " + std::to_string(good_tx_candidates_count) + " accepted and " + std::to_string(removed_transactions_ids.size()) + " removed");

            std::optional<std::array<uint8_t,32>> txid0_le;
            if (!non_cb_txs.empty()) {
                if (private_mempool_entries.size() > 0) {
                    txid0_le = hex256_le(private_mempool_entries[0].txid);
                }
                else if (result["transactions"][0].contains("txid"))
                {
                    txid0_le = hex256_le(result["transactions"][0]["txid"].get<std::string>());
                }
            }

            blkasm::GbtBlockTemplateData in;
            in.version      = result.at("version").get<int32_t>();
            in.prev_hash_le = hex256_le(result.at("previousblockhash").get<std::string>());
            in.curtime      = result.at("curtime").get<uint32_t>();
            in.bits_u32     = parse_bits_be_to_uint32_t(result.at("bits").get<std::string>());
            in.coinbase_value = collected_fees;
            in.height       = result.at("height").get<int32_t>();
            if (result.contains("coinbaseaux") && result["coinbaseaux"].contains("flags")) {
                in.coinbase_flags = hex_decode(result["coinbaseaux"]["flags"].get<std::string>());
            }
            in.segwit_active = contains_rule(result,"segwit") || result.contains("default_witness_commitment");;
            in.non_cb_tx_bytes = non_cb_txs;
            in.first_non_cb_expected_txid_le = txid0_le;

            // Assemble full block (no network/RPC dependencies here)
            auto block_proposal = blkasm::assemble_block_proposal(in, payout_script);

            // Propose the block
            auto blk_hex = hex_encode(block_proposal.block_bytes);

            LOG_INFO("New block proposal assembled");

            nlohmann::json res = rpc.get_blocktemplate(BlockTemplateMode::PROPOSAL, rules, blk_hex);
            crow::response r(200);
            r.set_header("Content-Type", "application/json");
            if (res.is_null())
            {
                r.write("Valid Block");
            } else {
                r.write(res.dump());
            }

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
