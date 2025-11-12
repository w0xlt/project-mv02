#include "utils/sys_utils.h"

int32_t get_time() {
    auto now = std::chrono::system_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            now.time_since_epoch())
            .count());
}