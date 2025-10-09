#pragma once

#include <string>
#include <vector>
#include <array>
#include <cstddef>

// Convert hex string to bytes
std::vector<std::byte> from_hex(const std::string& hex);

// Convert bytes to hex string
std::string bytes_to_hex(const std::vector<std::byte>& bytes);

// Convert txid bytes to hex (reversed for display)
std::string txid_to_hex_reversed(const std::array<std::byte, 32>& txid_bytes);

// Check if transaction has witness flag
bool has_witness_flag(const std::vector<std::byte>& tx_bytes);
