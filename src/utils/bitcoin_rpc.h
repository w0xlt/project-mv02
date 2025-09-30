#pragma once

#include <string>
#include <nlohmann/json.hpp>

// Bitcoin RPC client
class BitcoinRPC {
public:
    explicit BitcoinRPC(const std::string& rpc_url = "http://127.0.0.1:8332/");
    
    // Call gettxout RPC method
    nlohmann::json get_txout(const std::string& txid, int vout, bool include_mempool);
    
    // Convert BTC to satoshis
    static uint64_t btc_to_sats(const nlohmann::json& jnum);

private:
    std::string rpc_url_;
    
    // Read authentication cookie
    static std::string read_cookie();
    
    // Split "user:pass" into two strings
    static std::pair<std::string, std::string> split_userpass(const std::string& up);
};
