// MIT License (see LICENSE)

#pragma once
#include <cstdint>

namespace sha256mini {

struct CpuFeatures {
    bool avx2{false};
    bool shani{false};
};

CpuFeatures DetectCpuFeatures();

} // namespace sha256mini
