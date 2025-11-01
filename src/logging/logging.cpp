#include "logging/logging.h"
#include "kernel/bitcoinkernel_wrapper.h"
#include <ctime>
#include <memory>

// Kernel log handler implementation
class KernelLogHandler {
public:
    void LogMessage(std::string_view message) {
        // Kernel messages already include category, level, and formatting
        std::cout << message;  // No newline - kernel includes it
    }
};

std::string get_timestamp() {
    auto now = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    return std::string(buf);
}

// Global kernel logger - internal to this translation unit
static std::unique_ptr<btck::Logger<KernelLogHandler>> g_kernel_logger;

void setup_kernel_logging() {
    // Enable kernel logging categories we're interested in
    btck::logging_enable_category(btck::LogCategory::VALIDATION);
    btck::logging_enable_category(btck::LogCategory::KERNEL);
    
    // Set log level
    btck::logging_set_level_category(btck::LogCategory::ALL, btck::LogLevel::DEBUG_LEVEL);
    
    // Create logging options
    btck_LoggingOptions log_opts = {
        .log_timestamps = 1,
        .log_time_micros = 0,
        .log_threadnames = 0,
        .log_sourcelocations = 0,
        .always_print_category_levels = 1
    };
    
    // Create the kernel logger
    auto handler = std::make_unique<KernelLogHandler>();
    g_kernel_logger = std::make_unique<btck::Logger<KernelLogHandler>>(
        std::move(handler)
    );
    
    LOG_INFO("Bitcoinkernel logging initialized");
}

void cleanup_kernel_logging() {
    LOG_INFO("Shutting down kernel logging...");
    g_kernel_logger.reset();
}
