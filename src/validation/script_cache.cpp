#include "validation/script_cache.h"
#include "logging/logging.h"
#include "utils/hex_utils.h"
#include <sstream>
#include <iomanip>

ScriptTypeCache g_script_type_cache;

std::string ScriptTypeCache::get_or_compute(const std::string& spk_hex, const std::vector<std::byte>& script) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Check cache first
    auto it = cache_.find(spk_hex);
    if (it != cache_.end()) {
        hits_++;
        return it->second;
    }
    
    // Cache miss - compute the script type
    misses_++;
    std::string script_type = compute_script_type(script);
    
    // Add to cache if not full
    if (cache_.size() < MAX_CACHE_SIZE) {
        cache_[spk_hex] = script_type;
    } else {
        LOG_DEBUG("Script type cache full, skipping cache insertion");
    }
    
    return script_type;
}

void ScriptTypeCache::print_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t total = hits_ + misses_;
    if (total > 0) {
        double hit_rate = (static_cast<double>(hits_) / total) * 100.0;
        std::ostringstream stats;
        stats << "Script type cache stats - Hits: " << hits_ 
              << ", Misses: " << misses_ 
              << ", Hit rate: " << std::fixed << std::setprecision(2) << hit_rate 
              << "%, Size: " << cache_.size();
        LOG_INFO(stats.str());
    }
}

void ScriptTypeCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
    hits_ = 0;
    misses_ = 0;
    LOG_INFO("Script type cache cleared");
}

size_t ScriptTypeCache::get_size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.size();
}

std::string ScriptTypeCache::compute_script_type(const std::vector<std::byte>& script) {
    if (script.size() == 22 && script[0] == std::byte{0x00} && script[1] == std::byte{0x14}) {
        return "witness_v0_keyhash (P2WPKH)";
    } else if (script.size() == 34 && script[0] == std::byte{0x00} && script[1] == std::byte{0x20}) {
        return "witness_v0_scripthash (P2WSH)";
    } else if (script.size() == 34 && script[0] == std::byte{0x51} && script[1] == std::byte{0x20}) {
        return "witness_v1_taproot (P2TR)";
    } else if (script.size() == 23 && script[0] == std::byte{0xa9} && script[1] == std::byte{0x14} && script[22] == std::byte{0x87}) {
        return "p2sh";
    } else if (script.size() == 25 && script[0] == std::byte{0x76} && script[1] == std::byte{0xa9}) {
        return "p2pkh";
    } else if (script.size() >= 2 && script[0] <= std::byte{0x51}) {
        int version = -1;
        if (script[0] == std::byte{0x00}) version = 0;
        else if (script[0] >= std::byte{0x51} && script[0] <= std::byte{0x60}) {
            version = static_cast<int>(script[0]) - 0x50;
        }

        if (version >= 0) {
            return "witness_v" + std::to_string(version) + " (size=" + std::to_string(script.size()) + ")";
        }
    }
    return "unknown (size=" + std::to_string(script.size()) + ", first_bytes=" +
            (script.size() >= 2 ? bytes_to_hex({script[0], script[1]}) : "N/A") + ")";
}
