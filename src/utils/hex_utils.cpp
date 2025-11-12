#include "utils/hex_utils.h"
#include <cctype>
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <vector>
#include <cstdint>
#include <array>

std::vector<std::byte> from_hex(const std::string& hex) {
    auto nib = [](char c) -> int {
        if ('0' <= c && c <= '9') return c - '0';
        c = std::tolower(static_cast<unsigned char>(c));
        if ('a' <= c && c <= 'f') return 10 + (c - 'a');
        return -1;
    };
    
    std::vector<std::byte> out;
    int hi = -1;
    for (char c : hex) {
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        int v = nib(c);
        if (v < 0) throw std::runtime_error("non-hex character");
        if (hi < 0) {
            hi = v;
        } else {
            out.push_back(static_cast<std::byte>((hi << 4) | v));
            hi = -1;
        }
    }
    if (hi >= 0) throw std::runtime_error("odd-length hex");
    return out;
}

std::string bytes_to_hex(const std::vector<std::byte>& bytes) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (std::byte b : bytes) {
        oss << std::setw(2) << static_cast<int>(static_cast<unsigned char>(b));
    }
    return oss.str();
}

std::string txid_to_hex_reversed(const std::array<std::byte, 32>& txid_bytes) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (auto it = txid_bytes.rbegin(); it != txid_bytes.rend(); ++it) {
        oss << std::setw(2) << static_cast<int>(static_cast<unsigned char>(*it));
    }
    return oss.str();
}

bool has_witness_flag(const std::vector<std::byte>& tx_bytes) {
    if (tx_bytes.size() > 6) {
        return (tx_bytes[4] == std::byte{0x00} && tx_bytes[5] == std::byte{0x01});
    }
    return false;
}

/*
std::array<uint8_t, 4> int32_to_little_endian(int32_t value) {
    return {
        static_cast<uint8_t>(value & 0xFF),
        static_cast<uint8_t>((value >> 8) & 0xFF),
        static_cast<uint8_t>((value >> 16) & 0xFF),
        static_cast<uint8_t>((value >> 24) & 0xFF)
    };
}

std::vector<uint8_t> serialize_compact_size(uint64_t value) {
    std::vector<uint8_t> result;

    if (value < 0xFD) {
        result.push_back(static_cast<uint8_t>(value));
    } else if (value <= 0xFFFF) {
        result.push_back(0xFD);
        result.push_back(static_cast<uint8_t>(value & 0xFF));
        result.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    } else if (value <= 0xFFFFFFFF) {
        result.push_back(0xFE);
        for (int i = 0; i < 4; ++i)
            result.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFF));
    } else {
        result.push_back(0xFF);
        for (int i = 0; i < 8; ++i)
            result.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFF));
    }

    return result;
}
*/