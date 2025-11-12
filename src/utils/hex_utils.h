#pragma once

#include <string>
#include <vector>
#include <array>
#include <cstddef>
#include <cstdint>

// Convert hex string to bytes
std::vector<std::byte> from_hex(const std::string& hex);

// Convert bytes to hex string
std::string bytes_to_hex(const std::vector<std::byte>& bytes);

// Convert txid bytes to hex (reversed for display)
std::string txid_to_hex_reversed(const std::array<std::byte, 32>& txid_bytes);

// Check if transaction has witness flag
bool has_witness_flag(const std::vector<std::byte>& tx_bytes);

// Convert int32_t to little endian
//std::array<uint8_t, 4> int32_to_little_endian(int32_t value);

// Serialize int64_t to compact size representation
// https://learnmeabitcoin.com/technical/general/compact-size/
//std::vector<uint8_t> serialize_compact_size(uint64_t value);