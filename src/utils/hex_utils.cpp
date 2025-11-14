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

std::string hex_encode(const std::vector<uint8_t>& v) {
    static const char* k = "0123456789abcdef";
    std::string s; s.reserve(v.size()*2);
    for (uint8_t b : v) { s.push_back(k[b>>4]); s.push_back(k[b&0xF]); }
    return s;
}

std::vector<uint8_t> hex_decode(const std::string& h) {
    auto nyb = [](char c)->int{
        if (c>='0'&&c<='9') return c-'0';
        if (c>='a'&&c<='f') return c-'a'+10;
        if (c>='A'&&c<='F') return c-'A'+10;
        return -1;
    };
    if (h.size()%2) throw std::runtime_error("hex odd length");
    std::vector<uint8_t> out; out.reserve(h.size()/2);
    for (size_t i=0;i<h.size();i+=2) {
        int hi=nyb(h[i]), lo=nyb(h[i+1]);
        if (hi<0||lo<0) throw std::runtime_error("hex invalid");
        out.push_back(uint8_t((hi<<4)|lo));
    }
    return out;
}
// Print a 32-byte little-endian hash in user-facing big-endian hex (RPC style)
std::string hex_rev(const std::array<uint8_t,32>& h_le) {
    static const char* k = "0123456789abcdef";
    std::string s; s.reserve(64);
    for (int i=31;i>=0;--i) { uint8_t b=h_le[i]; s.push_back(k[b>>4]); s.push_back(k[b&0xF]); }
    return s;
}

std::array<uint8_t,32> hex256_le(const std::string& hex_be) {
    auto v = hex_decode(hex_be);
    if (v.size()!=32) throw std::runtime_error("hex256_le expects 32 bytes");
    std::array<uint8_t,32> a{}; for (int i=0;i<32;++i) a[i]=v[31-i]; return a;
}

uint32_t parse_bits_be_to_uint32_t(const std::string& bits_hex_be) {
    auto v = hex_decode(bits_hex_be);
    if (v.size()!=4) throw std::runtime_error("bits must be 4 bytes");
    return (uint32_t(v[0])<<24)|(uint32_t(v[1])<<16)|(uint32_t(v[2])<<8)|uint32_t(v[3]); // write LE later
}