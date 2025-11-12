// MIT License (see LICENSE)

#pragma once
#include <cstdint>
#include <cstring>

namespace sha256mini {

static inline uint32_t ReadBE32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

static inline void WriteBE32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}

static inline void WriteBE32State(uint8_t* out, const uint32_t s[8]) {
    for (int i = 0; i < 8; ++i) WriteBE32(out + 4*i, s[i]);
}

} // namespace sha256mini
