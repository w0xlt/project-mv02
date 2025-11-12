#pragma once
#include <cstdint>
#include <vector>
#include <array>
#include <optional>

namespace blkasm {

// Minimal fixed-size aliases to keep the header self-contained.
using u8  = std::uint8_t;

/** Inputs extracted from Bitcoin Core's getblocktemplate (template mode). */
struct GbtBlockTemplateData {
    // Header fields
    std::int32_t version{};
    std::array<u8,32> prev_hash_le{};   // LITTLE-ENDIAN bytes
    std::uint32_t curtime{};
    std::uint32_t bits_u32{};           // as parsed by parse_bits_be_to_u32

    // Coinbase fields
    std::int64_t coinbase_value{};
    std::int32_t height{};
    std::vector<u8> coinbase_flags;     // coinbaseaux.flags decoded from hex (may be empty)

    bool segwit_active{false};

    // Non-coinbase transactions (full raw bytes as returned by GBT "data")
    std::vector<std::vector<u8>> non_cb_tx_bytes;

    // Optional expected txid (LITTLE-ENDIAN) for calibration of hash byte order.
    std::optional<std::array<u8,32>> first_non_cb_expected_txid_le;
};

/** Result of assembling a full block ready for "proposal" mode. */
struct BlockAssemblyResult {
    std::vector<u8> block_bytes;                               // Full serialized block
    std::array<u8,32> coinbase_txid_le{};                      // txid of the coinbase (LE bytes)
    std::vector<std::array<u8,32>> txids_from_bytes_le;        // txids computed from bytes (LE)
    std::array<u8,32> merkle_root_le{};                        // txids merkle root (LE)
};

/**
 * Assemble a full block (including coinbase, optional segwit commitment, header+txs)
 * from the provided GBT data and payout script. Behavior replicates the original logic.
 */
BlockAssemblyResult assemble_block_proposal(const GbtBlockTemplateData& in,
                                            const std::vector<u8>& payout_script);

/** Utility exposed for diagnostics (same semantics as in the original code). */
std::array<u8,32> merkle_root(std::vector<std::array<u8,32>> hashes_le);

} // namespace blkasm