#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <cstddef>

// Cache for script type detection to avoid redundant pattern matching
class ScriptTypeCache {
public:
    static constexpr size_t MAX_CACHE_SIZE = 10000;
    
    // Get script type from cache or compute it
    std::string get_or_compute(const std::string& spk_hex, const std::vector<std::byte>& script);
    
    // Print cache statistics
    void print_stats() const;
    
    // Clear the cache
    void clear();
    
    // Get statistics
    size_t get_hits() const { return hits_; }
    size_t get_misses() const { return misses_; }
    size_t get_size() const;

private:
    std::unordered_map<std::string, std::string> cache_;
    mutable std::mutex mutex_;
    size_t hits_ = 0;
    size_t misses_ = 0;
    
    // Actual script type detection logic
    static std::string compute_script_type(const std::vector<std::byte>& script);
};

// Global script type cache instance
extern ScriptTypeCache g_script_type_cache;
