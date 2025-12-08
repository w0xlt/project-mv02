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

// Convert bytes (uint8_T) <-to-> string
std::string hex_encode(const std::vector<uint8_t>& v);
std::vector<uint8_t> hex_decode(const std::string& h);

// Convert txid bytes to hex (reversed for display)
std::string txid_to_hex_reversed(const std::array<std::byte, 32>& txid_bytes);

// Check if transaction has witness flag
bool has_witness_flag(const std::vector<std::byte>& tx_bytes);


// Print a 32-byte little-endian hash in user-facing big-endian hex (RPC style)
std::string hex_rev(const std::array<uint8_t,32>& h_le);

std::array<uint8_t,32> hex256_le(const std::string& hex_be);

uint32_t parse_bits_be_to_uint32_t(const std::string& bits_hex_be);