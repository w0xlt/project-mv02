#pragma once

#include <string>
#include <stdexcept>
#include <cstdint>
#include "kernel/bitcoinkernel_wrapper.h"
#include "utils/bitcoin_rpc.h"

// Validation result structure
struct ValidationResult {
    std::string txid;
    int64_t fee_sats;
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

// Opaque type for script verification status (actual type in .cpp)
using ScriptVerifyStatusType = int32_t;

// Validation error exception with status
class ValidationError : public std::runtime_error {
public:
    ScriptVerifyStatusType status;
    size_t input_index;

    ValidationError(const std::string& msg, ScriptVerifyStatusType s, size_t idx);
};

// Convert status enum to string for debugging
std::string status_to_string(ScriptVerifyStatusType status);

// Main validation function
ValidationResult validate_transaction(const std::string& tx_hex);

// Helper to collect input validation data
std::vector<InputValidationData> collect_input_data(const btck::Transaction& tx, BitcoinRPC& rpc);