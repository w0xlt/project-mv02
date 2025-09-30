#pragma once

#include <string>
#include <iostream>

// Simple timestamp helper
std::string get_timestamp();

// Simple logging macros
#define LOG_DEBUG(msg) std::cout << "[" << get_timestamp() << "] [DEBUG] " << msg << std::endl
#define LOG_INFO(msg)  std::cout << "[" << get_timestamp() << "] [INFO]  " << msg << std::endl
#define LOG_WARN(msg)  std::cout << "[" << get_timestamp() << "] [WARN]  " << msg << std::endl
#define LOG_ERROR(msg) std::cerr << "[" << get_timestamp() << "] [ERROR] " << msg << std::endl

// Setup kernel logging
void setup_kernel_logging();

// Cleanup kernel logging (call before exit)
void cleanup_kernel_logging();
