#pragma once

#include <string>
#include <nlohmann/json.hpp>


struct BlockTemplateMode {
    static constexpr std::string_view TEMPLATE = "template";
    static constexpr std::string_view PROPOSAL = "proposal";
    static constexpr std::string_view OMITTED  = "omitted";
};

struct BlockTemplateRules {
    static constexpr std::string_view SEGWIT = "segwit";
};


// Bitcoin RPC client
class BitcoinRPC {
public:
    explicit BitcoinRPC(const std::string& rpc_url = "http://127.0.0.1:48332/");

    // Call gettxout RPC method
    nlohmann::json get_txout(const std::string& txid, int vout, bool include_mempool);

    // Call getrawtransaction RPC method
    nlohmann::json get_rawtransaction(const std::string& txid, int verbose=2);

    // Call getblocktemplate PRC method
    nlohmann::json get_blocktemplate(std::string_view blocktemplate_mode, const nlohmann::json& blocktemplate_rules, const std::string& block_data = "");

    // Convert BTC to satoshis
    static uint64_t btc_to_sats(const nlohmann::json& jnum);

private:
    std::string rpc_url_;

    // Read authentication cookie
    static std::string read_cookie();

    // Process a Bitcoin RPC request
    nlohmann::json process_request(const nlohmann::json& body);

    // Split "user:pass" into two strings
    static std::pair<std::string, std::string> split_userpass(const std::string& up);
};
