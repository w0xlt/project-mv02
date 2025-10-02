#include <crow.h>
#include <nlohmann/json.hpp>
#include <cpr/cpr.h>
#include <string>
#include <vector>
#include <cctype>
#include <stdexcept>
#include <cstdio>
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

static uint64_t btc_to_sats_exact(const nlohmann::json& jnum) {
    // Turn the JSON number into its textual form (e.g., 0.123 -> "0.123")
    std::string s = jnum.dump();          // dump never quotes numbers
    // Normalize
    auto dot = s.find('.');
    std::string intp = (dot == std::string::npos) ? s : s.substr(0, dot);
    std::string frac = (dot == std::string::npos) ? "" : s.substr(dot + 1);

    // Remove optional leading '+', handle sign if you want (amounts should be >=0)
    if (!intp.empty() && intp[0] == '+') intp.erase(0, 1);

    // Truncate/pad to exactly 8 fractional digits
    if (frac.size() > 8) frac.resize(8);
    while (frac.size() < 8) frac.push_back('0');

    // Edge cases like ".5" or "-.5"
    if (intp.empty() || intp == "-") intp += "0";

    // Convert
    uint64_t sats = 0;
    // (Amounts from gettxout are non-negative)
    sats = std::stoull(intp) * 100'000'000ULL + (frac.empty() ? 0ULL : std::stoull(frac));
    return sats;
}

int main() {
    crow::SimpleApp app;

    // POST /verify with JSON: { "tx": "...hex...", "spk": "...hex...", "amount": 12345 }
    CROW_ROUTE(app, "/verify").methods("POST"_method)([](const crow::request& req){
        auto body = crow::json::load(req.body);
        if (!body || !body.has("tx_hex")) {
            return crow::response(400, "Missing tx/spk/amount");
        }

        try {
            std::string tx_hex = body["tx_hex"].s();
            // std::string spk_hex = body["spk"].s();
            // int64_t amount = body["amount"].i();

            std::vector<unsigned char> tx_bytes  = from_hex(tx_hex);
            // std::vector<unsigned char> spk_bytes = from_hex(spk_hex);

            btck_Transaction* tx = btck_transaction_create(tx_bytes.data(), tx_bytes.size());
            if (!tx) return crow::response(400, "tx parse failed");

            size_t input_count = btck_transaction_count_inputs(tx);

            // storage for per-input artifacts
            std::vector<btck_ScriptPubkey*>            spks;
            std::vector<const btck_TransactionOutput*> outs_c_array;  // const view for API
            std::vector<uint64_t>                      amounts_sats;  // value per input (sats)

            spks.reserve(input_count);
            outs_c_array.reserve(input_count);
            amounts_sats.reserve(input_count);

            bool ok = true;

            printf("input_count: %lu\n", input_count);

            for (size_t i = 0; i < input_count; i++) {
                const btck_TransactionInput* input = btck_transaction_get_input_at(tx, i);

                const btck_TransactionOutPoint* out_point = btck_transaction_input_get_out_point(input);

                const btck_Txid* out_point_txid = btck_transaction_out_point_get_txid(out_point);
                
                uint32_t out_point_index = btck_transaction_out_point_get_index(out_point);

                std::vector<unsigned char> txid_bytes(32, 0);
                btck_txid_to_bytes(out_point_txid, txid_bytes.data());
                std::string out_point_txid_hex = TxidToHexReversed(txid_bytes);

                printf("txid: %s, n: %u\n", out_point_txid_hex.c_str(), out_point_index);

                // Query UTXO set (typically include_mempool=false for pure UTXO set)
                nlohmann::json result = rpc_call_gettxout(out_point_txid_hex, out_point_index, /*include_mempool=*/false);
                if (result.is_null() || !result.contains("scriptPubKey") ||
                    !result["scriptPubKey"].contains("hex") || !result.contains("value")) {
                    fprintf(stderr, "Missing prevout data for %s:%u\n", out_point_txid_hex.c_str(), out_point_index);
                    ok = false; break;
                }

                std::string spk_hex = result["scriptPubKey"]["hex"].get<std::string>();
                std::vector<unsigned char> spk_bytes = from_hex(spk_hex);
                btck_ScriptPubkey* spk = btck_script_pubkey_create(spk_bytes.data(), spk_bytes.size());
                if (!spk) {
                    fprintf(stderr, "scriptPubKey parse failed for %s:%u\n", out_point_txid_hex.c_str(), out_point_index);
                    ok = false; break;
                }

                // Parse value in sats (JSON returns BTC)
                uint64_t value_sats = btc_to_sats_exact(result["value"]);

                // Build a TxOut for the prevout
                btck_TransactionOutput* out = btck_transaction_output_create(spk, value_sats);
                if (!out) {
                    btck_script_pubkey_destroy(spk);
                    fprintf(stderr, "TransactionOutput create failed for %s:%u\n", out_point_txid_hex.c_str(), out_point_index);
                    ok = false; break;
                }

                // Store
                spks.push_back(spk);
                outs_c_array.push_back(out);   // const view
                amounts_sats.push_back(value_sats);
            }

            if (!ok) {
                // cleanup if we failed mid-way
                for (auto* s : spks)        if (s) btck_script_pubkey_destroy(s);
                btck_transaction_destroy(tx);
                return crow::response(400, "failed to build prevouts");
            }

            // === Verification pass: one call per input ===
            for (size_t i = 0; i < input_count; ++i) {
                const btck_ScriptPubkey* spk_i = spks[i];
                int64_t amount_i = static_cast<int64_t>(amounts_sats[i]); // API takes int64_t

                btck_ScriptVerifyStatus status{};
                unsigned int input_index = static_cast<unsigned int>(i);

                btck_ScriptVerificationFlags flags = btck_ScriptVerificationFlags_ALL;

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
                    printf("Input %zu verified\n", i);
                } else {
                    // status gives you the exact reason
                    fprintf(stderr, "Input %zu verify failed (rc=%d, status=%d)\n", i, rc, (int)status);
                }
            }

            // === Cleanup ===
            for (auto* s : spks)        btck_script_pubkey_destroy(s);
            btck_transaction_destroy(tx);

            crow::json::wvalue res;


            res["verification"] = "Test";

            return crow::response(200, res);

        } catch (const std::exception& e) {
            return crow::response(400, std::string("error: ") + e.what());
        }
    });

    register_routes(app);

    app.port(8080).multithreaded().run();
}
