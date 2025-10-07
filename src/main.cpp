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
#include "bitcoinkernel.h"

std::string TxidToHexReversed(const std::vector<unsigned char>& txid_bytes)
{
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');

    // iterate in reverse order
    for (auto it = txid_bytes.rbegin(); it != txid_bytes.rend(); ++it) {
        oss << std::setw(2) << static_cast<int>(*it);
    }
    return oss.str();
}

// Helper to convert bytes to hex for debugging
std::string bytes_to_hex(const std::vector<unsigned char>& bytes) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (unsigned char b : bytes) {
        oss << std::setw(2) << static_cast<int>(b);
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
    // JSON-RPC body
    nlohmann::json body = {
        {"jsonrpc","1.0"},
        {"id","crow"},
        {"method","gettxout"},
        {"params", { txid, vout, include_mempool }}
    };
    std::string body_str = body.dump();

    // Basic Auth from cookie
    auto up = split_userpass(read_cookie());

    // CPR POST
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
        return j["result"]; // may be null if spent
    } else {
        throw std::runtime_error("RPC error: " + j["error"].dump());
    }
}

// --- Crow route ---
// POST /gettxout {"txid":"<hex>", "vout":0, "include_mempool":true}
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
static std::vector<unsigned char> from_hex(const std::string& hex) {
    auto nib = [](char c)->int {
        if ('0' <= c && c <= '9') return c - '0';
        c = std::tolower(static_cast<unsigned char>(c));
        if ('a' <= c && c <= 'f') return 10 + (c - 'a');
        return -1;
    };
    std::vector<unsigned char> out;
    int hi = -1;
    for (char c : hex) {
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        int v = nib(c);
        if (v < 0) throw std::runtime_error("non-hex character");
        if (hi < 0) { hi = v; }
        else { out.push_back(static_cast<unsigned char>((hi << 4) | v)); hi = -1; }
    }
    if (hi >= 0) throw std::runtime_error("odd-length hex");
    return out;
}

static uint64_t btc_to_sats(const nlohmann::json& jnum) {
    std::string s = jnum.dump();
    if (s.find('e') != std::string::npos || s.find('E') != std::string::npos) {
        long double ld = std::strtold(s.c_str(), nullptr);
        return (uint64_t) llround(ld * 100000000.0L);
    }
    auto dot = s.find('.');
    std::string intp = (dot == std::string::npos) ? s : s.substr(0, dot);
    std::string frac = (dot == std::string::npos) ? "" : s.substr(dot + 1);
    if (intp.empty() || intp == "-") intp = "0";
    if (frac.size() > 8) frac.resize(8);
    while (frac.size() < 8) frac.push_back('0');
    uint64_t whole = std::stoull(intp);
    uint64_t frac8 = frac.empty() ? 0ULL : std::stoull(frac);
    return whole * 100000000ULL + frac8;
}

// Helper to detect if transaction has witness data based on flag
static bool has_witness_flag(const std::vector<unsigned char>& tx_bytes) {
    // Check for witness flag (bytes 4-5 should be 0x00 0x01 after version)
    if (tx_bytes.size() > 6) {
        return (tx_bytes[4] == 0x00 && tx_bytes[5] == 0x01);
    }
    return false;
}

// Helper to get script type description
static std::string get_script_type(const std::vector<unsigned char>& script) {
    if (script.size() == 22 && script[0] == 0x00 && script[1] == 0x14) {
        return "witness_v0_keyhash (P2WPKH)";
    } else if (script.size() == 34 && script[0] == 0x00 && script[1] == 0x20) {
        return "witness_v0_scripthash (P2WSH)";
    } else if (script.size() == 34 && script[0] == 0x51 && script[1] == 0x20) {
        return "witness_v1_taproot (P2TR)";
    } else if (script.size() == 23 && script[0] == 0xa9 && script[1] == 0x14 && script[22] == 0x87) {
        return "p2sh";
    } else if (script.size() == 25 && script[0] == 0x76 && script[1] == 0xa9) {
        return "p2pkh";
    } else if (script.size() >= 2 && script[0] <= 0x51) {
        // Check for other witness versions (v2-v16)
        int version = -1;
        if (script[0] == 0x00) version = 0;
        else if (script[0] >= 0x51 && script[0] <= 0x60) version = script[0] - 0x50;

        if (version >= 0) {
            return "witness_v" + std::to_string(version) + " (size=" + std::to_string(script.size()) + ")";
        }
    }
    return "unknown (size=" + std::to_string(script.size()) + ", first_bytes=" +
            (script.size() >= 2 ? bytes_to_hex({script[0], script[1]}) : "N/A") + ")";
}

// Convert status enum to string for debugging
static std::string status_to_string(btck_ScriptVerifyStatus status) {
    // These are common Bitcoin Core script verification error codes
    // The actual values may differ in bitcoinkernel
    switch(status) {
        case 0: return "OK_or_UNKNOWN (0)";
        case 1: return "EVAL_FALSE";
        case 2: return "OP_RETURN";
        case 3: return "SCRIPT_SIZE";
        case 4: return "PUSH_SIZE";
        case 5: return "OP_COUNT";
        case 6: return "STACK_SIZE";
        case 7: return "SIG_COUNT";
        case 8: return "PUBKEY_COUNT";
        case 9: return "VERIFY";
        case 10: return "EQUALVERIFY";
        case 11: return "CHECKMULTISIGVERIFY";
        case 12: return "CHECKSIGVERIFY";
        case 13: return "NUMEQUALVERIFY";
        case 14: return "BAD_OPCODE";
        case 15: return "DISABLED_OPCODE";
        case 16: return "INVALID_STACK_OPERATION";
        case 17: return "INVALID_ALTSTACK_OPERATION";
        case 18: return "UNBALANCED_CONDITIONAL";
        case 19: return "NEGATIVE_LOCKTIME";
        case 20: return "UNSATISFIED_LOCKTIME";
        case 21: return "SIG_HASHTYPE";
        case 22: return "SIG_DER";
        case 23: return "MINIMALDATA";
        case 24: return "SIG_PUSHONLY";
        case 25: return "SIG_HIGH_S";
        case 26: return "SIG_NULLDUMMY";
        case 27: return "PUBKEYTYPE";
        case 28: return "CLEANSTACK";
        case 29: return "MINIMALIF";
        case 30: return "SIG_NULLFAIL";
        case 31: return "DISCOURAGE_UPGRADABLE_NOPS";
        case 32: return "DISCOURAGE_UPGRADABLE_WITNESS_PROGRAM";
        case 33: return "WITNESS_PROGRAM_WRONG_LENGTH";
        case 34: return "WITNESS_PROGRAM_WITNESS_EMPTY";
        case 35: return "WITNESS_PROGRAM_MISMATCH";
        case 36: return "WITNESS_MALLEATED";
        case 37: return "WITNESS_MALLEATED_P2SH";
        case 38: return "WITNESS_UNEXPECTED";
        case 39: return "WITNESS_PUBKEYTYPE";
        case 40: return "CONST_SCRIPTCODE";
        case 41: return "TAPROOT_WRONG_CONTROL_SIZE";
        default: return "UNKNOWN_STATUS_" + std::to_string((int)status);
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
    btck_ScriptVerifyStatus status;
    size_t input_index;

    ValidationError(const std::string& msg, btck_ScriptVerifyStatus s, size_t idx)
        : std::runtime_error(msg), status(s), input_index(idx) {}
};

// Extracted validation function with debugging
static ValidationResult validate_transaction(std::string tx_hex) {
    std::vector<unsigned char> tx_bytes = from_hex(tx_hex);

    // Debug: Check for witness flag
    bool has_witness = has_witness_flag(tx_bytes);
    printf("\n=== TRANSACTION DEBUG INFO ===\n");
    printf("Transaction size: %zu bytes\n", tx_bytes.size());
    printf("First 12 bytes: ");
    for (size_t i = 0; i < std::min(size_t(12), tx_bytes.size()); i++) {
        printf("%02x", tx_bytes[i]);
    }
    printf("\n");
    printf("Has witness flag (0x0001): %s\n", has_witness ? "YES" : "NO");

    btck_Transaction* tx = btck_transaction_create(tx_bytes.data(), tx_bytes.size());
    if (!tx) {
        throw std::runtime_error("tx parse failed");
    }

    const btck_Txid* txid = btck_transaction_get_txid(tx);
    std::vector<unsigned char> txid_bytes(32, 0);
    btck_txid_to_bytes(txid, txid_bytes.data());
    std::string txid_hex = TxidToHexReversed(txid_bytes);
    printf("Verifying txid: %s\n", txid_hex.c_str());

    size_t input_count = btck_transaction_count_inputs(tx);
    printf("Input count: %zu\n", input_count);

    // storage for per-input artifacts
    std::vector<btck_ScriptPubkey*>            spks;
    std::vector<const btck_TransactionOutput*> outs_c_array;  // const view for API
    std::vector<uint64_t>                      amounts_sats;  // value per input (sats)

    spks.reserve(input_count);
    outs_c_array.reserve(input_count);
    amounts_sats.reserve(input_count);

    printf("\n=== PROCESSING INPUTS ===\n");

    for (size_t i = 0; i < input_count; i++) {
        printf("\n--- Input %zu ---\n", i);
        const btck_TransactionInput* input = btck_transaction_get_input_at(tx, i);

        const btck_TransactionOutPoint* out_point = btck_transaction_input_get_out_point(input);
        const btck_Txid* out_point_txid = btck_transaction_out_point_get_txid(out_point);
        uint32_t out_point_index = btck_transaction_out_point_get_index(out_point);

        std::vector<unsigned char> out_point_txid_bytes(32, 0);
        btck_txid_to_bytes(out_point_txid, out_point_txid_bytes.data());
        std::string out_point_txid_hex = TxidToHexReversed(out_point_txid_bytes);

        printf("Prevout: %s:%u\n", out_point_txid_hex.c_str(), out_point_index);

        // Query UTXO set (typically include_mempool=false for pure UTXO set)
        nlohmann::json result = rpc_call_gettxout(out_point_txid_hex, out_point_index, /*include_mempool=*/false);
        if (result.is_null() || !result.contains("scriptPubKey") ||
            !result["scriptPubKey"].contains("hex") || !result.contains("value")) {
            // Cleanup before throwing
            for (auto* s : spks) if (s) btck_script_pubkey_destroy(s);
            btck_transaction_destroy(tx);
            throw std::runtime_error("Missing prevout data for " + out_point_txid_hex + ":" + std::to_string(out_point_index));
        }

        std::string spk_hex = result["scriptPubKey"]["hex"].get<std::string>();
        std::vector<unsigned char> spk_bytes = from_hex(spk_hex);

        // Debug: Script type
        std::string script_type = get_script_type(spk_bytes);
        printf("ScriptPubKey hex: %s\n", spk_hex.c_str());
        printf("Script type: %s\n", script_type.c_str());

        // Warning for Taproot
        if (script_type.find("P2TR") != std::string::npos) {
            printf("⚠️  WARNING: This is a Taproot output - validation may fail\n");
        }

        btck_ScriptPubkey* spk = btck_script_pubkey_create(spk_bytes.data(), spk_bytes.size());
        if (!spk) {
            // Cleanup before throwing
            for (auto* s : spks) if (s) btck_script_pubkey_destroy(s);
            btck_transaction_destroy(tx);
            throw std::runtime_error("scriptPubKey parse failed for " + out_point_txid_hex + ":" + std::to_string(out_point_index));
        }

        // Parse value in sats (JSON returns BTC)
        uint64_t value_sats = btc_to_sats(result["value"]);
        printf("Value: %lu sats\n", value_sats);

        // Build a TxOut for the prevout
        btck_TransactionOutput* out = btck_transaction_output_create(spk, value_sats);
        if (!out) {
            btck_script_pubkey_destroy(spk);
            // Cleanup before throwing
            for (auto* s : spks) if (s) btck_script_pubkey_destroy(s);
            btck_transaction_destroy(tx);
            throw std::runtime_error("TransactionOutput create failed for " + out_point_txid_hex + ":" + std::to_string(out_point_index));
        }

        // Store
        spks.push_back(spk);
        outs_c_array.push_back(out);   // const view
        amounts_sats.push_back(value_sats);
    }

    printf("\n=== VERIFICATION PHASE ===\n");

    // Store script types for debugging
    std::vector<std::string> script_types;
    for (size_t i = 0; i < input_count; ++i) {
        // Get the original scriptPubKey bytes for type detection
        // We need to retrieve this from the prevout lookup
        const btck_TransactionInput* input = btck_transaction_get_input_at(tx, i);
        const btck_TransactionOutPoint* out_point = btck_transaction_input_get_out_point(input);
        const btck_Txid* out_point_txid = btck_transaction_out_point_get_txid(out_point);
        uint32_t out_point_index = btck_transaction_out_point_get_index(out_point);

        std::vector<unsigned char> out_point_txid_bytes(32, 0);
        btck_txid_to_bytes(out_point_txid, out_point_txid_bytes.data());
        std::string out_point_txid_hex = TxidToHexReversed(out_point_txid_bytes);

        nlohmann::json result = rpc_call_gettxout(out_point_txid_hex, out_point_index, false);
        std::string spk_hex = result["scriptPubKey"]["hex"].get<std::string>();
        std::vector<unsigned char> spk_bytes = from_hex(spk_hex);
        script_types.push_back(get_script_type(spk_bytes));
    }

    // === Verification pass: one call per input ===
    for (size_t i = 0; i < input_count; ++i) {
        const btck_ScriptPubkey* spk_i = spks[i];
        int64_t amount_i = static_cast<int64_t>(amounts_sats[i]); // API takes int64_t

        btck_ScriptVerifyStatus status{};
        unsigned int input_index = static_cast<unsigned int>(i);

        // Debug: Try different flag combinations
        btck_ScriptVerificationFlags flags = btck_ScriptVerificationFlags_ALL;

        printf("\nVerifying input %zu:\n", i);
        printf("  Amount: %ld sats\n", amount_i);
        printf("  Script type: %s\n", script_types[i].c_str());
        printf("  Flags: 0x%x (ALL)\n", flags);

        // Check if this is a Taproot input
        bool is_taproot = (script_types[i].find("P2TR") != std::string::npos);
        if (is_taproot) {
            printf("  WARNING: This is a Taproot (P2TR) input!\n");
            printf("  Note: Taproot requires specific validation support and flags.\n");
            printf("  The bitcoinkernel library may not fully support Taproot validation.\n");
        }

        int rc = btck_script_pubkey_verify(
            spk_i,
            amount_i,
            tx,
            outs_c_array.data(), outs_c_array.size(),
            input_index,
            flags,
            &status
        );

        if (rc) {
            // success for this input
            printf("  Result: SUCCESS\n");
        } else {
            // Log detailed failure info before cleanup
            printf("  Result: FAILED\n");
            printf("  Status code: %d (%s)\n", (int)status, status_to_string(status).c_str());
            printf("  Transaction has witness: %s\n", has_witness ? "YES" : "NO");

            if (is_taproot) {
                printf("  LIKELY CAUSE: Taproot validation failure\n");
                printf("  Taproot (BIP341/BIP342) may require:\n");
                printf("    - Schnorr signature validation (BIP340)\n");
                printf("    - Tapscript execution support\n");
                printf("    - Specific consensus flags for Taproot\n");
            }

            // Cleanup before throwing
            for (auto* s : spks) btck_script_pubkey_destroy(s);
            btck_transaction_destroy(tx);

            // Throw exception with status information
            std::string error_msg = "Input " + std::to_string(i) + " verify failed (status=" + status_to_string(status) + ")";
            if (is_taproot) {
                error_msg += " [TAPROOT INPUT]";
            }
            throw ValidationError(error_msg, status, i);
        }
    }

    printf("\n=== FEE CALCULATION ===\n");

    // === Compute fee (sum(inputs) - sum(outputs)) ===
    int64_t sum_inputs_sats = 0;
    for (size_t i = 0; i < amounts_sats.size(); ++i) {
        sum_inputs_sats += static_cast<int64_t>(amounts_sats[i]);
    }

    int64_t sum_outputs_sats = 0;
    size_t output_count = btck_transaction_count_outputs(tx);
    for (size_t i = 0; i < output_count; ++i) {
        const btck_TransactionOutput* out_i = btck_transaction_get_output_at(tx, i);
        int64_t amt_i = btck_transaction_output_get_amount(out_i);
        sum_outputs_sats += amt_i;
    }

    int64_t fee_sats = sum_inputs_sats - sum_outputs_sats;
    printf("Total inputs: %ld sats\n", sum_inputs_sats);
    printf("Total outputs: %ld sats\n", sum_outputs_sats);
    printf("Fee: %ld sats\n", fee_sats);

    // === Cleanup ===
    for (auto* s : spks) btck_script_pubkey_destroy(s);
    btck_transaction_destroy(tx);

    printf("\n=== VALIDATION COMPLETE ===\n\n");

    return ValidationResult{txid_hex, fee_sats};
}

int main() {
    crow::SimpleApp app;

    // POST /verify with JSON: { "tx": "...hex...", "spk": "...hex...", "amount": 12345 }
    CROW_ROUTE(app, "/verify").methods("POST"_method)([](const crow::request& req){
        auto body = crow::json::load(req.body);
        if (!body || !body.has("tx_hex")) {
            return crow::response(400, "Missing tx_hex");
        }

        try {
            std::string tx_hex = body["tx_hex"].s();

            // Call the extracted validation function
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
            error_res["status"] = (int)e.status;
            error_res["status_name"] = status_to_string(e.status);
            error_res["input_index"] = (int)e.input_index;

            // Check if error message contains TAPROOT indicator
            if (std::string(e.what()).find("TAPROOT") != std::string::npos) {
                error_res["is_taproot"] = true;
                error_res["note"] = "Taproot transactions may not be fully supported by bitcoinkernel";
            }

            return crow::response(400, error_res);
        } catch (const std::exception& e) {
            return crow::response(400, std::string("error: ") + e.what());
        }
    });

    register_routes(app);

    app.port(8080).multithreaded().run();
}