#include "utils/bitcoin_rpc.h"
#include <cpr/cpr.h>
#include <fstream>
#include <stdexcept>
#include <cmath>
#include <cstdlib>
#include <cassert>

BitcoinRPC::BitcoinRPC(const std::string& rpc_url) : rpc_url_(rpc_url) {}

std::string BitcoinRPC::read_cookie() {
    const char* home = std::getenv("HOME");
    std::string path = std::string(home ? home : "") + "/.bitcoin/.cookie";
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open ~/.bitcoin/.cookie");
    std::string s;
    std::getline(f, s);
    return s;
}

std::pair<std::string, std::string> BitcoinRPC::split_userpass(const std::string& up) {
    auto pos = up.find(':');
    if (pos == std::string::npos) throw std::runtime_error("invalid cookie format");
    return { up.substr(0, pos), up.substr(pos + 1) };
}

nlohmann::json BitcoinRPC::process_request(const nlohmann::json& body) {
    std::string body_str = body.dump();

    auto up = split_userpass(read_cookie());

    auto res = cpr::Post(
        cpr::Url{rpc_url_},
        cpr::Header{{"Content-Type", "application/json"}},
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

nlohmann::json BitcoinRPC::get_txout(const std::string& txid, int vout, bool include_mempool) {
    nlohmann::json body = {
        {"jsonrpc", "1.0"},
        {"id", "crow"},
        {"method", "gettxout"},
        {"params", { txid, vout, include_mempool }}
    };

    return process_request(body);
}

uint64_t BitcoinRPC::btc_to_sats(const nlohmann::json& jnum) {
    double btc_value = jnum.get<double>();
    return static_cast<uint64_t>(std::round(btc_value * 100000000.0));
}
