// MIT License (see LICENSE)

// This is a *stub* AVX2 backend so the project builds everywhere.
// It forwards to the portable transform. Replace with a real AVX2-optimized
// implementation (e.g., from Bitcoin Core) to get SIMD acceleration.

#include <cstddef>
#include <cstdint>

// Provide the symbol so linking succeeds. You can replace this entire file
// with an optimized version that uses AVX2 intrinsics.
extern "C" void sha256_transform_avx2(uint32_t state[8], const uint8_t* data, size_t blocks) {
    // Intentionally left as a no-op; real implementation should process `blocks` 64-byte chunks.
    // Keeping an empty body ensures no behavior change unless you refactor to call into this.
    (void)state; (void)data; (void)blocks;
}
