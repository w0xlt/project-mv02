#pragma once

#include <atomic>
#include <cstddef>

// Configuration for parallel script verification
class ParallelVerificationConfig {
public:
    // Minimum number of inputs to trigger parallel verification
    static constexpr size_t MIN_INPUTS_FOR_PARALLEL = 4;
    
    // Maximum number of threads to use (0 = auto-detect)
    size_t max_threads = 0;
    
    bool enabled = true;
    
    // Get effective thread count
    size_t get_thread_count() const;
    
    // Print statistics
    void print_stats() const;
    
    // Statistics tracking
    void record_parallel_validation(size_t num_inputs);
    void record_sequential_validation(size_t num_inputs);
    
    // Get statistics
    size_t get_parallel_validations() const { return parallel_validations_.load(); }
    size_t get_sequential_validations() const { return sequential_validations_.load(); }
    size_t get_total_inputs_parallel() const { return total_inputs_parallel_.load(); }
    size_t get_total_inputs_sequential() const { return total_inputs_sequential_.load(); }

private:
    std::atomic<size_t> parallel_validations_{0};
    std::atomic<size_t> sequential_validations_{0};
    std::atomic<size_t> total_inputs_parallel_{0};
    std::atomic<size_t> total_inputs_sequential_{0};
};

// Global parallel verification config
extern ParallelVerificationConfig g_parallel_config;
