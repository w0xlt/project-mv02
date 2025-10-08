#include <crow.h>
#include <nlohmann/json.hpp>
#include <cpr/cpr.h>
#include <string>
#include <vector>
#include <cctype>
#include <stdexcept>
#include <cstdio>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <iostream>
#include <memory>
#include <ctime>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <future>
#include <thread>
#include "kernel/bitcoinkernel_wrapper.h"

// ============================================================================
// LOGGING INFRASTRUCTURE
// ============================================================================

// Simple timestamp helper
static std::string get_timestamp() {
    auto now = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    return std::string(buf);
}

// Simple logging macros using cout - kernel does the heavy lifting
#define LOG_DEBUG(msg) std::cout << "[" << get_timestamp() << "] [DEBUG] " << msg << std::endl
#define LOG_INFO(msg)  std::cout << "[" << get_timestamp() << "] [INFO]  " << msg << std::endl
#define LOG_WARN(msg)  std::cout << "[" << get_timestamp() << "] [WARN]  " << msg << std::endl
#define LOG_ERROR(msg) std::cerr << "[" << get_timestamp() << "] [ERROR] " << msg << std::endl

// Kernel log handler - captures bitcoinkernel internal logs
class KernelLogHandler {
public:
    void LogMessage(std::string_view message) {
        // Kernel messages already include category, level, and formatting
        std::cout << message;  // No newline - kernel includes it
    }
};

// Global kernel logger - by existing, it captures all kernel internal logs
static std::unique_ptr<btck::Logger<KernelLogHandler>> g_kernel_logger;

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

// Helper to convert bytes to hex (for txid display - reversed)
std::string TxidToHexReversed(const std::array<std::byte, 32>& txid_bytes)
{
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (auto it = txid_bytes.rbegin(); it != txid_bytes.rend(); ++it) {
        oss << std::setw(2) << static_cast<int>(static_cast<unsigned char>(*it));
    }
    return oss.str();
}

// Helper to convert bytes to hex for debugging
std::string bytes_to_hex(const std::vector<std::byte>& bytes) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (std::byte b : bytes) {
        oss << std::setw(2) << static_cast<int>(static_cast<unsigned char>(b));
    }
    return oss.str();
}

// ============================================================================
// SCRIPT TYPE CACHE
// ============================================================================

// Cache for script type detection to avoid redundant pattern matching
struct ScriptTypeCache {
    std::unordered_map<std::string, std::string> cache;
    mutable std::mutex mutex;
    size_t hits = 0;
    size_t misses = 0;
    static constexpr size_t MAX_CACHE_SIZE = 10000;
    
    std::string get_or_compute(const std::string& spk_hex, const std::vector<std::byte>& script) {
        std::lock_guard<std::mutex> lock(mutex);
        
        // Check cache first
        auto it = cache.find(spk_hex);
        if (it != cache.end()) {
            hits++;
            return it->second;
        }
        
        // Cache miss - compute the script type
        misses++;
        std::string script_type = compute_script_type(script);
        
        // Add to cache if not full
        if (cache.size() < MAX_CACHE_SIZE) {
            cache[spk_hex] = script_type;
        } else {
            // Cache is full - could implement LRU here, but for now just skip
            LOG_DEBUG("Script type cache full, skipping cache insertion");
        }
        
        return script_type;
    }
    
    void print_stats() const {
        std::lock_guard<std::mutex> lock(mutex);
        size_t total = hits + misses;
        if (total > 0) {
            double hit_rate = (static_cast<double>(hits) / total) * 100.0;
            std::ostringstream stats;
            stats << "Script type cache stats - Hits: " << hits 
                  << ", Misses: " << misses 
                  << ", Hit rate: " << std::fixed << std::setprecision(2) << hit_rate 
                  << "%, Size: " << cache.size();
            LOG_INFO(stats.str());
        }
    }
    
    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        cache.clear();
        hits = 0;
        misses = 0;
        LOG_INFO("Script type cache cleared");
    }
    
private:
    // Actual script type detection logic (moved from get_script_type)
    static std::string compute_script_type(const std::vector<std::byte>& script) {
        if (script.size() == 22 && script[0] == std::byte{0x00} && script[1] == std::byte{0x14}) {
            return "witness_v0_keyhash (P2WPKH)";
        } else if (script.size() == 34 && script[0] == std::byte{0x00} && script[1] == std::byte{0x20}) {
            return "witness_v0_scripthash (P2WSH)";
        } else if (script.size() == 34 && script[0] == std::byte{0x51} && script[1] == std::byte{0x20}) {
            return "witness_v1_taproot (P2TR)";
        } else if (script.size() == 23 && script[0] == std::byte{0xa9} && script[1] == std::byte{0x14} && script[22] == std::byte{0x87}) {
            return "p2sh";
        } else if (script.size() == 25 && script[0] == std::byte{0x76} && script[1] == std::byte{0xa9}) {
            return "p2pkh";
        } else if (script.size() >= 2 && script[0] <= std::byte{0x51}) {
            int version = -1;
            if (script[0] == std::byte{0x00}) version = 0;
            else if (script[0] >= std::byte{0x51} && script[0] <= std::byte{0x60}) {
                version = static_cast<int>(script[0]) - 0x50;
            }

            if (version >= 0) {
                return "witness_v" + std::to_string(version) + " (size=" + std::to_string(script.size()) + ")";
            }
        }
        return "unknown (size=" + std::to_string(script.size()) + ", first_bytes=" +
                (script.size() >= 2 ? bytes_to_hex({script[0], script[1]}) : "N/A") + ")";
    }
};

// Global script type cache instance
static ScriptTypeCache g_script_type_cache;

// ============================================================================
// PARALLEL VERIFICATION CONFIGURATION
// ============================================================================

struct ParallelVerificationConfig {
    // Minimum number of inputs to trigger parallel verification
    // Below this threshold, sequential verification is faster due to thread overhead
    static constexpr size_t MIN_INPUTS_FOR_PARALLEL = 4;
    
    // Maximum number of threads to use for parallel verification
    // 0 = auto-detect based on hardware concurrency
    size_t max_threads = 0;
    
    bool enabled = true;
    
    // Statistics
    std::atomic<size_t> parallel_validations{0};
    std::atomic<size_t> sequential_validations{0};
    std::atomic<size_t> total_inputs_parallel{0};
    std::atomic<size_t> total_inputs_sequential{0};
    
    size_t get_thread_count() const {
        if (max_threads > 0) {
            return max_threads;
        }
        // Use hardware concurrency, but cap at reasonable limit
        unsigned int hw_threads = std::thread::hardware_concurrency();
        return hw_threads > 0 ? std::min(hw_threads, 16u) : 4u;
    }
    
    void print_stats() const {
        size_t total_validations = parallel_validations + sequential_validations;
        if (total_validations > 0) {
            double parallel_pct = (static_cast<double>(parallel_validations) / total_validations) * 100.0;
            std::ostringstream stats;
            stats << "Parallel verification stats - "
                  << "Parallel: " << parallel_validations << " txs (" << total_inputs_parallel << " inputs), "
                  << "Sequential: " << sequential_validations << " txs (" << total_inputs_sequential << " inputs), "
                  << "Parallel %: " << std::fixed << std::setprecision(2) << parallel_pct << "%";
            LOG_INFO(stats.str());
        }
    }
};

// Global parallel verification config
static ParallelVerificationConfig g_parallel_config;

// Structure to hold the result of a single input verification
struct InputVerificationResult {
    size_t input_index;
    bool success;
    btck::ScriptVerifyStatus status;
    std::string error_message;
    
    InputVerificationResult(size_t idx, bool ok, btck::ScriptVerifyStatus s, std::string err = "")
        : input_index(idx), success(ok), status(s), error_message(std::move(err)) {}
};

// ============================================================================
// MORE UTILITY FUNCTIONS
// ============================================================================

// Read ~/.bitcoin/.cookie -> "user:token"
static std::string read_cookie() {
    const char* home = std::getenv("HOME");
    std::string path = std::string(home ? home : "") + "/.bitcoin/.cookie";
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open ~/.bitcoin/.cookie");
    std::string s;
    std::getline(f, s);
    return s;
}

// Split "user:pass" into two strings
static std::pair<std::string, std::string> split_userpass(const std::string& up) {
    auto pos = up.find(':');
    if (pos == std::string::npos) throw std::runtime_error("invalid cookie format");
    return { up.substr(0, pos), up.substr(pos + 1) };
}

static nlohmann::json rpc_call_gettxout(const std::string& txid, int vout, bool include_mempool,
    const std::string& rpc_url = "http://127.0.0.1:8332/") {
    nlohmann::json body = {
        {"jsonrpc","1.0"},
        {"id","crow"},
        {"method","gettxout"},
        {"params", { txid, vout, include_mempool }}
    };
    std::string body_str = body.dump();

    auto up = split_userpass(read_cookie());

    auto res = cpr::Post(
        cpr::Url{rpc_url},
        cpr::Header{{"Content-Type","application/json"}},
        cpr::Authentication{up.first, up.second, cpr::AuthMode::BASIC},
        cpr::Body{body_str},
        cpr::Timeout{3000}
    );

    if (res.error.code != cpr::ErrorCode::OK) {
        throw std::runtime_error(std::string("cpr error: ") + res.error.message);
    }
    if (res.status_code != 200) {
        throw std::runtime_error("non-200 from bitcoind: " + std::to_string(res.status_code) +
            " body: " + res.text);
    }

    auto j = nlohmann::json::parse(res.text);
    if (!j.contains("error") || j["error"].is_null()) {
        return j["result"];
    } else {
        throw std::runtime_error("RPC error: " + j["error"].dump());
    }
}

// --- Crow route ---
static void register_routes(crow::SimpleApp& app) {
    CROW_ROUTE(app, "/gettxout").methods("POST"_method)(
    [](const crow::request& req){
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

            nlohmann::json result = rpc_call_gettxout(txid, vout, include_mempool);

            crow::response r(200);
            r.set_header("Content-Type", "application/json");
            r.write(result.dump());
            return r;
        } catch (const std::exception& e) {
            return crow::response(500, std::string("RPC failed: ") + e.what());
        }
    });
}

// Minimal hex -> bytes
static std::vector<std::byte> from_hex(const std::string& hex) {
    auto nib = [](char c)->int {
        if ('0' <= c && c <= '9') return c - '0';
        c = std::tolower(static_cast<unsigned char>(c));
        if ('a' <= c && c <= 'f') return 10 + (c - 'a');
        return -1;
    };
    std::vector<std::byte> out;
    int hi = -1;
    for (char c : hex) {
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        int v = nib(c);
        if (v < 0) throw std::runtime_error("non-hex character");
        if (hi < 0) { hi = v; }
        else { out.push_back(static_cast<std::byte>((hi << 4) | v)); hi = -1; }
    }
    if (hi >= 0) throw std::runtime_error("odd-length hex");
    return out;
}

static uint64_t btc_to_sats(const nlohmann::json& jnum) {
    double btc_value = jnum.get<double>();
    return static_cast<uint64_t>(std::round(btc_value * 100000000.0));
}

// Helper to detect if transaction has witness data based on flag
static bool has_witness_flag(const std::vector<std::byte>& tx_bytes) {
    if (tx_bytes.size() > 6) {
        return (tx_bytes[4] == std::byte{0x00} && tx_bytes[5] == std::byte{0x01});
    }
    return false;
}

// Convert status enum to string for debugging
static std::string status_to_string(btck::ScriptVerifyStatus status) {
    switch(status) {
        case btck::ScriptVerifyStatus::OK:
            return "OK";
        case btck::ScriptVerifyStatus::ERROR_INVALID_FLAGS_COMBINATION:
            return "ERROR_INVALID_FLAGS_COMBINATION";
        case btck::ScriptVerifyStatus::ERROR_SPENT_OUTPUTS_REQUIRED:
            return "ERROR_SPENT_OUTPUTS_REQUIRED";
        default:
            return "UNKNOWN_STATUS_" + std::to_string(static_cast<int>(status));
    }
}

// Validation result structure
struct ValidationResult {
    std::string txid;
    int64_t fee_sats;
};

// Validation error exception with status
class ValidationError : public std::runtime_error {
public:
    btck::ScriptVerifyStatus status;
    size_t input_index;

    ValidationError(const std::string& msg, btck::ScriptVerifyStatus s, size_t idx)
        : std::runtime_error(msg), status(s), input_index(idx) {}
};

// Structure to hold per-input validation data
struct InputValidationData {
    std::string prevout_txid;
    uint32_t prevout_index;
    std::string script_type;
    uint64_t amount_sats;
    btck::ScriptPubkey script_pubkey;
    btck::TransactionOutput tx_output;

    InputValidationData(std::string txid, uint32_t idx, std::string type, 
                       uint64_t amt, btck::ScriptPubkey spk, btck::TransactionOutput out)
        : prevout_txid(std::move(txid))
        , prevout_index(idx)
        , script_type(std::move(type))
        , amount_sats(amt)
        , script_pubkey(std::move(spk))
        , tx_output(std::move(out))
    {}
};

// Extracted validation function with:
// - Range API for cleaner iteration
// - Cached script type detection for performance
// - Parallel verification for transactions with many inputs (>=4)
static ValidationResult validate_transaction(std::string tx_hex) {
    std::vector<std::byte> tx_bytes = from_hex(tx_hex);

    // Debug: Check for witness flag
    bool has_witness = has_witness_flag(tx_bytes);
    
    LOG_INFO("=== TRANSACTION DEBUG INFO ===");
    
    std::ostringstream info;
    info << "Transaction size: " << tx_bytes.size() << " bytes";
    LOG_INFO(info.str());
    
    std::ostringstream first_bytes;
    first_bytes << "First 12 bytes: ";
    for (size_t i = 0; i < std::min(size_t(12), tx_bytes.size()); i++) {
        first_bytes << std::hex << std::setfill('0') << std::setw(2) 
                    << static_cast<int>(static_cast<unsigned char>(tx_bytes[i]));
    }
    LOG_DEBUG(first_bytes.str());
    
    std::ostringstream witness_info;
    witness_info << "Has witness flag (0x0001): " << (has_witness ? "YES" : "NO");
    LOG_DEBUG(witness_info.str());

    // Use btck::Transaction wrapper
    btck::Transaction tx(tx_bytes);

    // Get txid using wrapper API
    auto txid_view = tx.Txid();
    auto txid_bytes = txid_view.ToBytes();
    std::string txid_hex = TxidToHexReversed(txid_bytes);
    
    std::ostringstream txid_info;
    txid_info << "Verifying txid: " << txid_hex;
    LOG_INFO(txid_info.str());

    // Count inputs for informational logging
    size_t input_count = tx.CountInputs();
    std::ostringstream input_info;
    input_info << "Input count: " << input_count;
    LOG_INFO(input_info.str());

    // Storage for per-input validation data - single pass collection
    std::vector<InputValidationData> input_data;

    LOG_INFO("=== PROCESSING INPUTS ===");

    // Collect all input data in one pass using Range API
    // tx.Inputs() returns a range that can be iterated with range-based for loops
    size_t input_index = 0;
    for (const auto& input_view : tx.Inputs()) {
        std::ostringstream input_header;
        input_header << "--- Input " << input_index << " ---";
        LOG_DEBUG(input_header.str());
        
        auto out_point_view = input_view.OutPoint();
        auto out_point_txid_view = out_point_view.Txid();
        uint32_t out_point_index = out_point_view.index();

        auto out_point_txid_bytes = out_point_txid_view.ToBytes();
        std::string out_point_txid_hex = TxidToHexReversed(out_point_txid_bytes);

        std::ostringstream prevout_info;
        prevout_info << "Prevout: " << out_point_txid_hex << ":" << out_point_index;
        LOG_DEBUG(prevout_info.str());

        // Query UTXO set
        nlohmann::json result = rpc_call_gettxout(out_point_txid_hex, out_point_index, false);
        if (result.is_null() || !result.contains("scriptPubKey") ||
            !result["scriptPubKey"].contains("hex") || !result.contains("value")) {
            throw std::runtime_error("Missing prevout data for " + out_point_txid_hex + ":" + std::to_string(out_point_index));
        }

        std::string spk_hex = result["scriptPubKey"]["hex"].get<std::string>();
        std::vector<std::byte> spk_bytes = from_hex(spk_hex);

        // Determine script type using cache - avoids redundant pattern matching
        // Common script types (P2WPKH, P2WSH, etc.) are cached for performance
        std::string script_type = g_script_type_cache.get_or_compute(spk_hex, spk_bytes);
        
        std::ostringstream spk_info;
        spk_info << "ScriptPubKey hex: " << spk_hex;
        LOG_DEBUG(spk_info.str());
        
        std::ostringstream type_info;
        type_info << "Script type: " << script_type;
        LOG_DEBUG(type_info.str());

        // Create ScriptPubkey using wrapper
        btck::ScriptPubkey spk(spk_bytes);

        // Parse value in sats
        uint64_t value_sats = btc_to_sats(result["value"]);
        
        std::ostringstream value_info;
        value_info << "Value: " << value_sats << " sats";
        LOG_DEBUG(value_info.str());

        // Create TransactionOutput using wrapper
        btck::TransactionOutput tx_out(spk, static_cast<int64_t>(value_sats));

        // Store all data together - avoids redundant lookups
        input_data.emplace_back(
            std::move(out_point_txid_hex),
            out_point_index,
            std::move(script_type),
            value_sats,
            std::move(spk),
            std::move(tx_out)
        );
        
        input_index++;
    }

    LOG_INFO("=== VERIFICATION PHASE ===");

    // Build spent_outputs vector from collected data
    std::vector<btck::TransactionOutput> spent_outputs;
    spent_outputs.reserve(input_count);
    for (const auto& data : input_data) {
        spent_outputs.push_back(btck::TransactionOutput(data.tx_output));
    }

    // Decide whether to use parallel or sequential verification
    bool use_parallel = g_parallel_config.enabled && 
                       input_count >= ParallelVerificationConfig::MIN_INPUTS_FOR_PARALLEL;
    
    if (use_parallel) {
        g_parallel_config.parallel_validations++;
        g_parallel_config.total_inputs_parallel += input_count;
        
        std::ostringstream parallel_info;
        parallel_info << "Using parallel verification with " << g_parallel_config.get_thread_count() 
                     << " threads for " << input_count << " inputs";
        LOG_INFO(parallel_info.str());
    } else {
        g_parallel_config.sequential_validations++;
        g_parallel_config.total_inputs_sequential += input_count;
    }

    // Verification pass: parallel or sequential based on input count
    std::vector<InputVerificationResult> verification_results;
    
    if (use_parallel) {
        // Parallel verification using std::async
        // Each input verification is completely independent and thread-safe
        // The Transaction and spent_outputs are const and can be safely shared across threads
        std::vector<std::future<InputVerificationResult>> futures;
        futures.reserve(input_count);
        
        // Launch verification tasks
        for (size_t i = 0; i < input_count; ++i) {
            futures.push_back(std::async(std::launch::async, [&, i]() -> InputVerificationResult {
                const auto& data = input_data[i];
                btck::ScriptVerifyStatus status = btck::ScriptVerifyStatus::OK;
                
                // Use all verification flags
                btck::ScriptVerificationFlags flags = btck::ScriptVerificationFlags::ALL;
                
                // Verify this input
                bool result = data.script_pubkey.Verify(
                    static_cast<int64_t>(data.amount_sats),
                    tx,
                    std::span<const btck::TransactionOutput>(spent_outputs),
                    static_cast<unsigned int>(i),
                    flags,
                    status
                );
                
                if (!result) {
                    std::ostringstream err;
                    err << "Input " << i << " verify failed (status=" << status_to_string(status) << ")";
                    return InputVerificationResult(i, false, status, err.str());
                }
                
                return InputVerificationResult(i, true, status);
            }));
        }
        
        // Collect results
        for (auto& future : futures) {
            verification_results.push_back(future.get());
        }
        
        // Log results in order
        for (const auto& result : verification_results) {
            size_t i = result.input_index;
            const auto& data = input_data[i];
            
            std::ostringstream verify_header;
            verify_header << "Verifying input " << i << ":";
            LOG_DEBUG(verify_header.str());
            
            std::ostringstream amount_info;
            amount_info << "  Amount: " << data.amount_sats << " sats";
            LOG_DEBUG(amount_info.str());
            
            std::ostringstream type_info;
            type_info << "  Script type: " << data.script_type;
            LOG_DEBUG(type_info.str());
            
            std::ostringstream flags_info;
            flags_info << "  Flags: 0x" << std::hex << static_cast<unsigned int>(btck::ScriptVerificationFlags::ALL) << " (ALL)";
            LOG_DEBUG(flags_info.str());
            
            if (result.success) {
                LOG_DEBUG("  Result: SUCCESS");
            } else {
                LOG_ERROR("  Result: FAILED");
                
                std::ostringstream error_details;
                error_details << "  Status code: " << static_cast<int>(result.status) 
                             << " (" << status_to_string(result.status) << ")";
                LOG_ERROR(error_details.str());
                
                std::ostringstream witness_err;
                witness_err << "  Transaction has witness: " << (has_witness ? "YES" : "NO");
                LOG_ERROR(witness_err.str());
                
                throw ValidationError(result.error_message, result.status, i);
            }
        }
        
    } else {
        // Sequential verification (original logic)
        for (size_t i = 0; i < input_count; ++i) {
            const auto& data = input_data[i];
            
            btck::ScriptVerifyStatus status = btck::ScriptVerifyStatus::OK;
            unsigned int input_index = static_cast<unsigned int>(i);

            // Use all verification flags
            btck::ScriptVerificationFlags flags = btck::ScriptVerificationFlags::ALL;

            std::ostringstream verify_header;
            verify_header << "Verifying input " << i << ":";
            LOG_DEBUG(verify_header.str());
            
            std::ostringstream amount_info;
            amount_info << "  Amount: " << data.amount_sats << " sats";
            LOG_DEBUG(amount_info.str());
            
            std::ostringstream type_info;
            type_info << "  Script type: " << data.script_type;
            LOG_DEBUG(type_info.str());
            
            std::ostringstream flags_info;
            flags_info << "  Flags: 0x" << std::hex << static_cast<unsigned int>(flags) << " (ALL)";
            LOG_DEBUG(flags_info.str());

            // Use wrapper's Verify method with std::span
            bool result = data.script_pubkey.Verify(
                static_cast<int64_t>(data.amount_sats),
                tx,
                std::span<const btck::TransactionOutput>(spent_outputs),
                input_index,
                flags,
                status
            );

            if (result) {
                LOG_DEBUG("  Result: SUCCESS");
            } else {
                LOG_ERROR("  Result: FAILED");
                
                std::ostringstream error_details;
                error_details << "  Status code: " << static_cast<int>(status) 
                             << " (" << status_to_string(status) << ")";
                LOG_ERROR(error_details.str());
                
                std::ostringstream witness_err;
                witness_err << "  Transaction has witness: " << (has_witness ? "YES" : "NO");
                LOG_ERROR(witness_err.str());

                std::string error_msg = "Input " + std::to_string(i) + " verify failed (status=" + status_to_string(status) + ")";
                throw ValidationError(error_msg, status, i);
            }
        }
    }

    LOG_INFO("=== FEE CALCULATION ===");

    // Compute fee (sum(inputs) - sum(outputs))
    int64_t sum_inputs_sats = 0;
    for (const auto& data : input_data) {
        sum_inputs_sats += static_cast<int64_t>(data.amount_sats);
    }

    // Use Range API for cleaner output iteration
    // tx.Outputs() provides a modern C++ range-based interface
    int64_t sum_outputs_sats = 0;
    for (const auto& output : tx.Outputs()) {
        sum_outputs_sats += output.Amount();
    }

    int64_t fee_sats = sum_inputs_sats - sum_outputs_sats;
    
    std::ostringstream fee_info;
    fee_info << "Total inputs: " << sum_inputs_sats << " sats";
    LOG_INFO(fee_info.str());
    
    std::ostringstream out_info;
    out_info << "Total outputs: " << sum_outputs_sats << " sats";
    LOG_INFO(out_info.str());
    
    std::ostringstream final_fee;
    final_fee << "Fee: " << fee_sats << " sats";
    LOG_INFO(final_fee.str());

    LOG_INFO("=== VALIDATION COMPLETE ===\n");

    return ValidationResult{txid_hex, fee_sats};
}

void setup_kernel_logging() {
    // Enable kernel logging categories we're interested in
    btck::logging_enable_category(btck::LogCategory::VALIDATION);
    btck::logging_enable_category(btck::LogCategory::KERNEL);
    
    // Set log level - you can adjust this to control verbosity
    // Available levels: TRACE_LEVEL, DEBUG_LEVEL, INFO_LEVEL
    btck::logging_set_level_category(btck::LogCategory::ALL, btck::LogLevel::DEBUG_LEVEL);
    
    // Create logging options
    btck_LoggingOptions log_opts = {
        .log_timestamps = 1,                   // Include timestamps
        .log_time_micros = 0,                  // Don't need microsecond precision
        .log_threadnames = 0,                  // Don't need thread names for now
        .log_sourcelocations = 0,              // Don't need source locations
        .always_print_category_levels = 1      // Always show category and level
    };
    
    // Create the kernel logger - by existing, it captures all kernel logs
    auto handler = std::make_unique<KernelLogHandler>();
    g_kernel_logger = std::make_unique<btck::Logger<KernelLogHandler>>(
        std::move(handler), 
        log_opts
    );
    
    LOG_INFO("Bitcoinkernel logging initialized");
}

int main() {
    // Initialize logging
    LOG_INFO("Starting Bitcoin Transaction Validator");
    
    // Set up kernel logging - this captures internal validation logs from bitcoinkernel
    // The g_kernel_logger object, by existing, routes all kernel logs through KernelLogHandler
    setup_kernel_logging();
    
    // Log parallel verification configuration
    std::ostringstream parallel_info;
    parallel_info << "Parallel verification: " << (g_parallel_config.enabled ? "ENABLED" : "DISABLED")
                  << ", Threshold: " << ParallelVerificationConfig::MIN_INPUTS_FOR_PARALLEL << " inputs"
                  << ", Threads: " << g_parallel_config.get_thread_count();
    LOG_INFO(parallel_info.str());
    
    crow::SimpleApp app;

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
            error_res["status"] = static_cast<int>(e.status);
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

    // GET /cache-stats - returns script type cache statistics
    CROW_ROUTE(app, "/cache-stats").methods("GET"_method)(
    [](){
        std::lock_guard<std::mutex> lock(g_script_type_cache.mutex);
        
        size_t total = g_script_type_cache.hits + g_script_type_cache.misses;
        double hit_rate = 0.0;
        if (total > 0) {
            hit_rate = (static_cast<double>(g_script_type_cache.hits) / total) * 100.0;
        }
        
        crow::json::wvalue stats;
        stats["cache_hits"] = g_script_type_cache.hits;
        stats["cache_misses"] = g_script_type_cache.misses;
        stats["total_lookups"] = total;
        stats["hit_rate_percent"] = hit_rate;
        stats["cache_size"] = g_script_type_cache.cache.size();
        stats["cache_max_size"] = ScriptTypeCache::MAX_CACHE_SIZE;
        
        return crow::response(200, stats);
    });

    // POST /cache-clear - clears the script type cache
    CROW_ROUTE(app, "/cache-clear").methods("POST"_method)(
    [](){
        g_script_type_cache.clear();
        
        crow::json::wvalue res;
        res["message"] = "Script type cache cleared successfully";
        
        return crow::response(200, res);
    });

    // GET /parallel-stats - returns parallel verification statistics
    CROW_ROUTE(app, "/parallel-stats").methods("GET"_method)(
    [](){
        size_t total_validations = g_parallel_config.parallel_validations + g_parallel_config.sequential_validations;
        double parallel_pct = 0.0;
        if (total_validations > 0) {
            parallel_pct = (static_cast<double>(g_parallel_config.parallel_validations) / total_validations) * 100.0;
        }
        
        crow::json::wvalue stats;
        stats["enabled"] = g_parallel_config.enabled;
        stats["min_inputs_threshold"] = ParallelVerificationConfig::MIN_INPUTS_FOR_PARALLEL;
        stats["max_threads"] = g_parallel_config.max_threads;
        stats["detected_threads"] = g_parallel_config.get_thread_count();
        stats["parallel_validations"] = g_parallel_config.parallel_validations.load();
        stats["sequential_validations"] = g_parallel_config.sequential_validations.load();
        stats["total_validations"] = total_validations;
        stats["parallel_percent"] = parallel_pct;
        stats["total_inputs_parallel"] = g_parallel_config.total_inputs_parallel.load();
        stats["total_inputs_sequential"] = g_parallel_config.total_inputs_sequential.load();
        
        return crow::response(200, stats);
    });

    // POST /parallel-config - configure parallel verification
    CROW_ROUTE(app, "/parallel-config").methods("POST"_method)(
    [](const crow::request& req){
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

    register_routes(app);

    LOG_INFO("Starting HTTP server on port 8080");
    app.port(8080).multithreaded().run();
    
    // Print statistics before shutdown
    LOG_INFO("Server stopped, printing final statistics:");
    g_script_type_cache.print_stats();
    g_parallel_config.print_stats();
    
    // Clean up kernel logger before exit to prevent shutdown assertion failures
    LOG_INFO("Shutting down...");
    g_kernel_logger.reset();
    
    return 0;
}