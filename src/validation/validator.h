#pragma once

#include <string>
#include <stdexcept>
#include <cstdint>

// Validation result structure
struct ValidationResult {
    std::string txid;
    int64_t fee_sats;
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
