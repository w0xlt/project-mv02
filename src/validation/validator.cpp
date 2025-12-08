#include "validation/validator.h"
#include "validation/script_cache.h"
#include "validation/parallel_config.h"
#include "utils/hex_utils.h"
#include "logging/logging.h"
#include <vector>
#include <sstream>
#include <iomanip>
#include <future>
#include <span>

// ValidationError constructor implementation
ValidationError::ValidationError(const std::string& msg, ScriptVerifyStatusType s, size_t idx)
    : std::runtime_error(msg), status(s), input_index(idx) {}

// Structure to hold the result of a single input verification
struct InputVerificationResult {
    size_t input_index;
    bool success;
    btck::ScriptVerifyStatus status;
    std::string error_message;
    
    InputVerificationResult(size_t idx, bool ok, btck::ScriptVerifyStatus s, std::string err = "")
        : input_index(idx), success(ok), status(s), error_message(std::move(err)) {}
};

std::string status_to_string(ScriptVerifyStatusType status) {
    // Cast to the actual kernel enum type
    auto kernel_status = static_cast<btck::ScriptVerifyStatus>(status);
    
    switch(kernel_status) {
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

// Helper to log transaction debug info
static void log_transaction_info(const std::vector<std::byte>& tx_bytes) {
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
}

// Helper to collect input validation data
std::vector<InputValidationData> collect_input_data(
    const btck::Transaction& tx, 
    BitcoinRPC& rpc) 
{
    std::vector<InputValidationData> input_data;
    
    LOG_INFO("=== PROCESSING INPUTS ===");
    
    size_t input_index = 0;
    for (const auto& input_view : tx.Inputs()) {
        std::ostringstream input_header;
        input_header << "--- Input " << input_index << " ---";
        LOG_DEBUG(input_header.str());
        
        auto out_point_view = input_view.OutPoint();
        auto out_point_txid_view = out_point_view.Txid();
        uint32_t out_point_index = out_point_view.index();

        auto out_point_txid_bytes = out_point_txid_view.ToBytes();
        std::string out_point_txid_hex = txid_to_hex_reversed(out_point_txid_bytes);

        std::ostringstream prevout_info;
        prevout_info << "Prevout: " << out_point_txid_hex << ":" << out_point_index;
        LOG_DEBUG(prevout_info.str());

        std::string spk_hex;
        uint64_t value_sats;
        // Query UTXO set
        nlohmann::json result = rpc.get_txout(out_point_txid_hex, out_point_index, false);
        if (result.is_null() || !result.contains("scriptPubKey") || !result["scriptPubKey"].contains("hex")
         || !result.contains("value"))
        {
            nlohmann::json rawtx_result = rpc.get_rawtransaction(out_point_txid_hex, /*verbose=*/2);
            if (rawtx_result.contains("error") && !rawtx_result["error"].is_null()) {
                throw std::runtime_error("Missing prevout data for " + out_point_txid_hex + 
                                   ":" + std::to_string(out_point_index));
            }
            spk_hex = rawtx_result["vout"][out_point_index]["scriptPubKey"]["hex"].get<std::string>();
            value_sats = BitcoinRPC::btc_to_sats(rawtx_result["vout"][out_point_index]["value"]);

        } else {
            spk_hex = result["scriptPubKey"]["hex"].get<std::string>();
            value_sats = BitcoinRPC::btc_to_sats(result["value"]);
        }

        std::vector<std::byte> spk_bytes = from_hex(spk_hex);

        // Determine script type using cache
        std::string script_type = g_script_type_cache.get_or_compute(spk_hex, spk_bytes);
        
        std::ostringstream spk_info;
        spk_info << "ScriptPubKey hex: " << spk_hex;
        LOG_DEBUG(spk_info.str());
        
        std::ostringstream type_info;
        type_info << "Script type: " << script_type;
        LOG_DEBUG(type_info.str());

        // Create ScriptPubkey using wrapper
        btck::ScriptPubkey spk(spk_bytes);

        std::ostringstream value_info;
        value_info << "Value: " << value_sats << " sats";
        LOG_DEBUG(value_info.str());

        // Create TransactionOutput using wrapper
        btck::TransactionOutput tx_out(spk, static_cast<int64_t>(value_sats));

        // Store all data together
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
    
    return input_data;
}

// Helper to verify inputs sequentially
static void verify_inputs_sequential(
    const btck::Transaction& tx,
    const std::vector<InputValidationData>& input_data,
    const std::vector<btck::TransactionOutput>& spent_outputs,
    bool has_witness)
{
    for (size_t i = 0; i < input_data.size(); ++i) {
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
                         << " (" << status_to_string(static_cast<int32_t>(status)) << ")";
            LOG_ERROR(error_details.str());
            
            std::ostringstream witness_err;
            witness_err << "  Transaction has witness: " << (has_witness ? "YES" : "NO");
            LOG_ERROR(witness_err.str());

            std::string error_msg = "Input " + std::to_string(i) + 
                                   " verify failed (status=" + status_to_string(static_cast<int32_t>(status)) + ")";
            throw ValidationError(error_msg, static_cast<int32_t>(status), i);
        }
    }
}

// Helper to verify inputs in parallel
static void verify_inputs_parallel(
    const btck::Transaction& tx,
    const std::vector<InputValidationData>& input_data,
    const std::vector<btck::TransactionOutput>& spent_outputs,
    bool has_witness)
{
    size_t input_count = input_data.size();
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
                err << "Input " << i << " verify failed (status=" << status_to_string(static_cast<int32_t>(status)) << ")";
                return InputVerificationResult(i, false, status, err.str());
            }
            
            return InputVerificationResult(i, true, status);
        }));
    }
    
    // Collect results
    std::vector<InputVerificationResult> verification_results;
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
                         << " (" << status_to_string(static_cast<int32_t>(result.status)) << ")";
            LOG_ERROR(error_details.str());
            
            std::ostringstream witness_err;
            witness_err << "  Transaction has witness: " << (has_witness ? "YES" : "NO");
            LOG_ERROR(witness_err.str());
            
                            throw ValidationError(result.error_message, static_cast<int32_t>(result.status), i);
        }
    }
}

ValidationResult validate_transaction(const std::string& tx_hex) {
    std::vector<std::byte> tx_bytes = from_hex(tx_hex);
    bool has_witness = has_witness_flag(tx_bytes);
    
    log_transaction_info(tx_bytes);

    // Use btck::Transaction wrapper
    btck::Transaction tx(tx_bytes);

    // Get txid using wrapper API
    auto txid_view = tx.Txid();
    auto txid_bytes = txid_view.ToBytes();
    std::string txid_hex = txid_to_hex_reversed(txid_bytes);
    
    std::ostringstream txid_info;
    txid_info << "Verifying txid: " << txid_hex;
    LOG_INFO(txid_info.str());

    // Count inputs
    size_t input_count = tx.CountInputs();
    std::ostringstream input_info;
    input_info << "Input count: " << input_count;
    LOG_INFO(input_info.str());

    // Collect all input data
    BitcoinRPC rpc;
    std::vector<InputValidationData> input_data = collect_input_data(tx, rpc);

    LOG_INFO("=== VERIFICATION PHASE ===");

    // Build spent_outputs vector
    std::vector<btck::TransactionOutput> spent_outputs;
    spent_outputs.reserve(input_count);
    for (const auto& data : input_data) {
        spent_outputs.push_back(btck::TransactionOutput(data.tx_output));
    }

    // Decide whether to use parallel or sequential verification
    bool use_parallel = g_parallel_config.enabled && 
                       input_count >= ParallelVerificationConfig::MIN_INPUTS_FOR_PARALLEL;
    
    if (use_parallel) {
        g_parallel_config.record_parallel_validation(input_count);
        
        std::ostringstream parallel_info;
        parallel_info << "Using parallel verification with " << g_parallel_config.get_thread_count() 
                     << " threads for " << input_count << " inputs";
        LOG_INFO(parallel_info.str());
        
        verify_inputs_parallel(tx, input_data, spent_outputs, has_witness);
    } else {
        g_parallel_config.record_sequential_validation(input_count);
        verify_inputs_sequential(tx, input_data, spent_outputs, has_witness);
    }

    LOG_INFO("=== FEE CALCULATION ===");

    // Compute fee (sum(inputs) - sum(outputs))
    int64_t sum_inputs_sats = 0;
    for (const auto& data : input_data) {
        sum_inputs_sats += static_cast<int64_t>(data.amount_sats);
    }

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