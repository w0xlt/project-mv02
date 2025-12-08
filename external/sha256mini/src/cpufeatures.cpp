// MIT License (see LICENSE)

#include <sha256mini/cpufeatures.h>
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386) || defined(_M_IX86)
  #if defined(_MSC_VER)
    #include <intrin.h>
  #else
    #include <cpuid.h>
  #endif
#endif

namespace sha256mini {

static inline void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386) || defined(_M_IX86)
  #if defined(_MSC_VER)
    int regs[4];
    __cpuidex(regs, (int)leaf, (int)subleaf);
    a = (uint32_t)regs[0]; b = (uint32_t)regs[1]; c = (uint32_t)regs[2]; d = (uint32_t)regs[3];
  #else
    __cpuid_count(leaf, subleaf, a, b, c, d);
  #endif
#else
  a = b = c = d = 0;
#endif
}

CpuFeatures DetectCpuFeatures() {
    CpuFeatures f{};
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386) || defined(_M_IX86)
    uint32_t a,b,c,d;
    // AVX2: leaf 7, subleaf 0, EBX bit 5
    cpuid(7, 0, a, b, c, d);
    f.avx2 = (b & (1u << 5)) != 0;
    // SHA: leaf 7, subleaf 0, EBX bit 29 (Intel SHA extensions)
    f.shani = (b & (1u << 29)) != 0;
#endif
    return f;
}

} // namespace sha256mini
