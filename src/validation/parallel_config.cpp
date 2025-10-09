#include "validation/parallel_config.h"
#include "logging/logging.h"
#include <thread>
#include <algorithm>
#include <sstream>
#include <iomanip>

ParallelVerificationConfig g_parallel_config;

size_t ParallelVerificationConfig::get_thread_count() const {
    if (max_threads > 0) {
        return max_threads;
    }
    // Use hardware concurrency, but cap at reasonable limit
    unsigned int hw_threads = std::thread::hardware_concurrency();
    return hw_threads > 0 ? std::min(hw_threads, 16u) : 4u;
}

void ParallelVerificationConfig::print_stats() const {
    size_t total_validations = parallel_validations_ + sequential_validations_;
    if (total_validations > 0) {
        double parallel_pct = (static_cast<double>(parallel_validations_) / total_validations) * 100.0;
        std::ostringstream stats;
        stats << "Parallel verification stats - "
              << "Parallel: " << parallel_validations_ << " txs (" << total_inputs_parallel_ << " inputs), "
              << "Sequential: " << sequential_validations_ << " txs (" << total_inputs_sequential_ << " inputs), "
              << "Parallel %: " << std::fixed << std::setprecision(2) << parallel_pct << "%";
        LOG_INFO(stats.str());
    }
}

void ParallelVerificationConfig::record_parallel_validation(size_t num_inputs) {
    parallel_validations_++;
    total_inputs_parallel_ += num_inputs;
}

void ParallelVerificationConfig::record_sequential_validation(size_t num_inputs) {
    sequential_validations_++;
    total_inputs_sequential_ += num_inputs;
}
