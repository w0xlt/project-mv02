// MIT License (see LICENSE)

#include <sha256mini/sha256.h>
#include <sha256mini/common.h>
#include <cstring>

namespace sha256mini {

static inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static inline uint32_t Ch(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); }
static inline uint32_t Maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }
static inline uint32_t Sigma0(uint32_t x) { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); }
static inline uint32_t Sigma1(uint32_t x) { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); }
static inline uint32_t sigma0(uint32_t x) { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); }
static inline uint32_t sigma1(uint32_t x) { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); }

static const uint32_t K[64] = {
    0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
    0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
    0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
    0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
    0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
    0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
    0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
    0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U
};

SHA256::SHA256() { Reset(); }

void SHA256::Reset() {
    m_len_bits = 0;
    m_state[0]=0x6a09e667U; m_state[1]=0xbb67ae85U; m_state[2]=0x3c6ef372U; m_state[3]=0xa54ff53aU;
    m_state[4]=0x510e527fU; m_state[5]=0x9b05688cU; m_state[6]=0x1f83d9abU; m_state[7]=0x5be0cd19U;
    m_buf_len = 0;
}

void SHA256::Update(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    m_len_bits += (uint64_t)len * 8;
    if (m_buf_len) {
        size_t take = (len < (64 - m_buf_len)) ? len : (64 - m_buf_len);
        std::memcpy(m_buf + m_buf_len, p, take);
        m_buf_len += take; p += take; len -= take;
        if (m_buf_len == 64) {
            Transform(m_buf, 1);
            m_buf_len = 0;
        }
    }
    while (len >= 64) {
        Transform(p, 1);
        p += 64; len -= 64;
    }
    if (len) {
        std::memcpy(m_buf, p, len);
        m_buf_len = len;
    }
}

void SHA256::Final(uint8_t out[32]) {
    uint8_t pad[128]; // enough for padding
    size_t pad_len = 0;
    // append 0x80
    pad[pad_len++] = 0x80;
    size_t rem = (m_buf_len + 1) % 64;
    size_t zeros = (rem <= 56) ? (56 - rem) : (56 + 64 - rem);
    std::memset(pad + pad_len, 0, zeros);
    pad_len += zeros;
    // append length (big-endian)
    uint8_t len_be[8];
    for (int i = 0; i < 8; ++i) len_be[7 - i] = uint8_t((m_len_bits >> (i*8)) & 0xFF);
    Update(pad, pad_len);
    Update(len_be, 8);
    // output
    WriteBE32State(out, m_state);
    // reset internal buffer len to avoid accidental reuse
    m_buf_len = 0;
}

void SHA256::Transform(const uint8_t* data, size_t blocks) {
    uint32_t a,b,c,d,e,f,g,h;
    uint32_t W[64];
    for (size_t blk = 0; blk < blocks; ++blk) {
        for (int i = 0; i < 16; ++i) W[i] = ReadBE32(data + 4*i);
        for (int i = 16; i < 64; ++i) W[i] = sigma1(W[i-2]) + W[i-7] + sigma0(W[i-15]) + W[i-16];
        a=m_state[0]; b=m_state[1]; c=m_state[2]; d=m_state[3];
        e=m_state[4]; f=m_state[5]; g=m_state[6]; h=m_state[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t T1 = h + Sigma1(e) + Ch(e,f,g) + K[i] + W[i];
            uint32_t T2 = Sigma0(a) + Maj(a,b,c);
            h = g; g = f; f = e; e = d + T1;
            d = c; c = b; b = a; a = T1 + T2;
        }
        m_state[0] += a; m_state[1] += b; m_state[2] += c; m_state[3] += d;
        m_state[4] += e; m_state[5] += f; m_state[6] += g; m_state[7] += h;
        data += 64;
    }
}

std::array<uint8_t,32> SHA256::Hash(const void* data, size_t len) {
    SHA256 ctx;
    ctx.Update(data, len);
    std::array<uint8_t,32> out{};
    ctx.Final(out.data());
    return out;
}

} // namespace sha256mini
