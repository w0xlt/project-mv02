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
#include "kernel/bitcoinkernel_wrapper.h"

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

// Helper to get script type description
static std::string get_script_type(const std::vector<std::byte>& script) {
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

// Extracted validation function with optimization
static ValidationResult validate_transaction(std::string tx_hex) {
    std::vector<std::byte> tx_bytes = from_hex(tx_hex);

    // Debug: Check for witness flag
    bool has_witness = has_witness_flag(tx_bytes);
    printf("\n=== TRANSACTION DEBUG INFO ===\n");
    printf("Transaction size: %zu bytes\n", tx_bytes.size());
    printf("First 12 bytes: ");
    for (size_t i = 0; i < std::min(size_t(12), tx_bytes.size()); i++) {
        printf("%02x", static_cast<unsigned char>(tx_bytes[i]));
    }
    printf("\n");
    printf("Has witness flag (0x0001): %s\n", has_witness ? "YES" : "NO");

    // Use btck::Transaction wrapper
    btck::Transaction tx(tx_bytes);

    // Get txid using wrapper API
    auto txid_view = tx.Txid();
    auto txid_bytes = txid_view.ToBytes();
    std::string txid_hex = TxidToHexReversed(txid_bytes);
    printf("Verifying txid: %s\n", txid_hex.c_str());

    size_t input_count = tx.CountInputs();
    printf("Input count: %zu\n", input_count);

    // Storage for per-input validation data - single pass collection
    std::vector<InputValidationData> input_data;
    input_data.reserve(input_count);

    printf("\n=== PROCESSING INPUTS ===\n");

    // Collect all input data in one pass
    for (size_t i = 0; i < input_count; i++) {
        printf("\n--- Input %zu ---\n", i);
        
        auto input_view = tx.GetInput(i);
        auto out_point_view = input_view.OutPoint();
        auto out_point_txid_view = out_point_view.Txid();
        uint32_t out_point_index = out_point_view.index();

        auto out_point_txid_bytes = out_point_txid_view.ToBytes();
        std::string out_point_txid_hex = TxidToHexReversed(out_point_txid_bytes);

        printf("Prevout: %s:%u\n", out_point_txid_hex.c_str(), out_point_index);

        // Query UTXO set
        nlohmann::json result = rpc_call_gettxout(out_point_txid_hex, out_point_index, false);
        if (result.is_null() || !result.contains("scriptPubKey") ||
            !result["scriptPubKey"].contains("hex") || !result.contains("value")) {
            throw std::runtime_error("Missing prevout data for " + out_point_txid_hex + ":" + std::to_string(out_point_index));
        }

        std::string spk_hex = result["scriptPubKey"]["hex"].get<std::string>();
        std::vector<std::byte> spk_bytes = from_hex(spk_hex);

        // Determine script type once
        std::string script_type = get_script_type(spk_bytes);
        printf("ScriptPubKey hex: %s\n", spk_hex.c_str());
        printf("Script type: %s\n", script_type.c_str());

        // Create ScriptPubkey using wrapper
        btck::ScriptPubkey spk(spk_bytes);

        // Parse value in sats
        uint64_t value_sats = btc_to_sats(result["value"]);
        printf("Value: %lu sats\n", value_sats);

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
    }

    printf("\n=== VERIFICATION PHASE ===\n");

    // Build spent_outputs vector from collected data
    std::vector<btck::TransactionOutput> spent_outputs;
    spent_outputs.reserve(input_count);
    for (const auto& data : input_data) {
        spent_outputs.push_back(btck::TransactionOutput(data.tx_output));
    }

    // Verification pass: one call per input
    for (size_t i = 0; i < input_count; ++i) {
        const auto& data = input_data[i];
        
        btck::ScriptVerifyStatus status = btck::ScriptVerifyStatus::OK;
        unsigned int input_index = static_cast<unsigned int>(i);

        // Use all verification flags
        btck::ScriptVerificationFlags flags = btck::ScriptVerificationFlags::ALL;

        printf("\nVerifying input %zu:\n", i);
        printf("  Amount: %lu sats\n", data.amount_sats);
        printf("  Script type: %s\n", data.script_type.c_str());
        printf("  Flags: 0x%x (ALL)\n", static_cast<unsigned int>(flags));

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
            printf("  Result: SUCCESS\n");
        } else {
            printf("  Result: FAILED\n");
            printf("  Status code: %d (%s)\n", static_cast<int>(status), status_to_string(status).c_str());
            printf("  Transaction has witness: %s\n", has_witness ? "YES" : "NO");

            std::string error_msg = "Input " + std::to_string(i) + " verify failed (status=" + status_to_string(status) + ")";
            throw ValidationError(error_msg, status, i);
        }
    }

    printf("\n=== FEE CALCULATION ===\n");

    // Compute fee (sum(inputs) - sum(outputs))
    int64_t sum_inputs_sats = 0;
    for (const auto& data : input_data) {
        sum_inputs_sats += static_cast<int64_t>(data.amount_sats);
    }

    int64_t sum_outputs_sats = 0;
    size_t output_count = tx.CountOutputs();
    for (size_t i = 0; i < output_count; ++i) {
        auto out_view = tx.GetOutput(i);
        sum_outputs_sats += out_view.Amount();
    }

    int64_t fee_sats = sum_inputs_sats - sum_outputs_sats;
    printf("Total inputs: %ld sats\n", sum_inputs_sats);
    printf("Total outputs: %ld sats\n", sum_outputs_sats);
    printf("Fee: %ld sats\n", fee_sats);

    printf("\n=== VALIDATION COMPLETE ===\n\n");

    return ValidationResult{txid_hex, fee_sats};
}

int main() {
    crow::SimpleApp app;

    // POST /verify with JSON: { "tx_hex": "...hex..." }
    CROW_ROUTE(app, "/verify").methods("POST"_method)([](const crow::request& req){
        auto body = crow::json::load(req.body);
        if (!body || !body.has("tx_hex")) {
            return crow::response(400, "Missing tx_hex");
        }

        try {
            std::string tx_hex = body["tx_hex"].s();

            // Call the validation function
            ValidationResult result = validate_transaction(tx_hex);

            // Build response
            crow::json::wvalue res;
            res["txid"] = result.txid;
            res["fee_sats"] = result.fee_sats;

            return crow::response(200, res);

        } catch (const ValidationError& e) {
            // Return validation error with status information
            crow::json::wvalue error_res;
            error_res["error"] = e.what();
            error_res["status"] = static_cast<int>(e.status);
            error_res["status_name"] = status_to_string(e.status);
            error_res["input_index"] = static_cast<int>(e.input_index);

            return crow::response(400, error_res);
        } catch (const std::exception& e) {
            return crow::response(400, std::string("error: ") + e.what());
        }
    });

    register_routes(app);

    app.port(8080).multithreaded().run();
}
