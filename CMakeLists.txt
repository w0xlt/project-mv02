cmake_minimum_required(VERSION 3.18)
project(bitcoinkernel_http C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Bitcoin Core build paths
# BITCOIN_DIR must be explicitly provided by the user (no default).
set(BITCOIN_DIR "" CACHE PATH "Bitcoin Core source tree")
if(NOT BITCOIN_DIR)
  message(FATAL_ERROR "You must set BITCOIN_DIR to the path of the Bitcoin Core source tree")
endif()

# BITCOIN_BUILD defaults to BITCOIN_DIR/build but can be overridden by the user.
set(BITCOIN_BUILD "${BITCOIN_DIR}/build" CACHE PATH "Bitcoin Core build tree (has lib/)")

# Find required packages from vcpkg
find_package(Crow CONFIG REQUIRED)
find_package(asio CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(cpr CONFIG REQUIRED)

# Source files organized by module
set(LOGGING_SOURCES
    src/logging/logging.cpp
)

set(UTILS_SOURCES
    src/utils/hex_utils.cpp
    src/utils/bitcoin_rpc.cpp
)

set(VALIDATION_SOURCES
    src/validation/script_cache.cpp
    src/validation/parallel_config.cpp
    src/validation/validator.cpp
)

set(MEMPOOL_SOURCES
    src/mempool/mempool.cpp
)

set(API_SOURCES
    src/api/routes.cpp
)

# Main executable
add_executable(bitcoinkernel_http
    src/main.cpp
    ${LOGGING_SOURCES}
    ${UTILS_SOURCES}
    ${VALIDATION_SOURCES}
    ${MEMPOOL_SOURCES}
    ${API_SOURCES}
)

# Include directories
target_include_directories(bitcoinkernel_http PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    "${BITCOIN_DIR}/src"
)

# Compile definitions
target_compile_definitions(bitcoinkernel_http PRIVATE BITCOINKERNEL_STATIC)

# Link libraries
target_link_libraries(bitcoinkernel_http PRIVATE
    "${BITCOIN_BUILD}/lib/libbitcoinkernel.a"
    Crow::Crow
    asio::asio
    nlohmann_json::nlohmann_json
    cpr::cpr
)

# Set RPATH for runtime linking
set_target_properties(bitcoinkernel_http PROPERTIES
    BUILD_RPATH "${BITCOIN_BUILD}/lib"
    INSTALL_RPATH "${BITCOIN_BUILD}/lib"
)

# Optional: Create a library for easier testing
add_library(bitcoinkernel_http_lib STATIC
    ${LOGGING_SOURCES}
    ${UTILS_SOURCES}
    ${VALIDATION_SOURCES}
    ${MEMPOOL_SOURCES}
    ${API_SOURCES}
)

target_include_directories(bitcoinkernel_http_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    "${BITCOIN_DIR}/src"
)

target_compile_definitions(bitcoinkernel_http_lib PUBLIC BITCOINKERNEL_STATIC)

target_link_libraries(bitcoinkernel_http_lib PUBLIC
    "${BITCOIN_BUILD}/lib/libbitcoinkernel.a"
    Crow::Crow
    asio::asio
    nlohmann_json::nlohmann_json
    cpr::cpr
)
