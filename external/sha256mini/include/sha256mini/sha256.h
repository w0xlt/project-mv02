// MIT License (see LICENSE)

#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace sha256mini {

class SHA256 {
public:
    SHA256();
    void Update(const void* data, size_t len);
    void Final(uint8_t out[32]);
    void Reset();

    // Convenience one-shot
    static std::array<uint8_t,32> Hash(const void* data, size_t len);
    static std::array<uint8_t,32> Hash(std::string_view s) {
        return Hash(s.data(), s.size());
    }

private:
    void Transform(const uint8_t* data, size_t blocks);

    uint64_t m_len_bits;
    uint32_t m_state[8];
    uint8_t  m_buf[64];
    size_t   m_buf_len;
};

} // namespace sha256mini
