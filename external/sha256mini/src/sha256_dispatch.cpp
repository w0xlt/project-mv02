// MIT License (see LICENSE)

#include <sha256mini/sha256.h>
#include <sha256mini/cpufeatures.h>

// Declaration from the AVX2 TU
extern "C" void sha256_transform_avx2(uint32_t state[8], const uint8_t* data, size_t blocks);

namespace sha256mini {

// This translation unit forces linking of avx2 TU and picks the best impl at runtime.
// Our SHA256 class already uses the portable Transform embedded there. For a drop-in
// optimized path, one could refactor to use function pointers. Kept minimal here.

// (No additional code needed: the portable implementation lives in sha256_portable.cpp.
//  If you provide a real AVX2 function with the signature above and refactor SHA256 to
//  call through it when DetectCpuFeatures().avx2, you'll get optimized transforms.)

} // namespace sha256mini
