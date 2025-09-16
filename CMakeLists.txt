cmake_minimum_required(VERSION 3.18)
project(bitcoinkernel_http C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Bitcoin Core build paths
# BITCOIN_DIR must be explicitly provided by the user (no default).
set(BITCOIN_DIR "" CACHE PATH "Bitcoin Core source tree")
if(NOT BITCOIN_DIR)
  message(FATAL_ERROR "You must set BITCOIN_DIR to the path of the Bitcoin Core source tree")
endif()

# BITCOIN_BUILD defaults to BITCOIN_DIR/build but can be overridden by the user.
set(BITCOIN_BUILD "${BITCOIN_DIR}/build" CACHE PATH "Bitcoin Core build tree (has lib/)")

# 🔧 Crow from vcpkg: note capital C and target name
find_package(Crow CONFIG REQUIRED)
# (asio is usually pulled transitively, but vcpkg suggests linking it explicitly)
find_package(asio CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(cpr CONFIG REQUIRED)

add_executable(bitcoinkernel_http src/main.cpp)

target_include_directories(bitcoinkernel_http PRIVATE
  "${BITCOIN_DIR}/src/kernel")

find_library(BITCOINKERNEL_LIB NAMES bitcoinkernel
  HINTS "${BITCOIN_BUILD}/lib" NO_DEFAULT_PATH REQUIRED)

target_link_libraries(bitcoinkernel_http PRIVATE
  "${BITCOINKERNEL_LIB}"
  Crow::Crow
  asio::asio
  nlohmann_json::nlohmann_json
  cpr::cpr
)

set_target_properties(bitcoinkernel_http PROPERTIES
  BUILD_RPATH "${BITCOIN_BUILD}/lib"
  INSTALL_RPATH "${BITCOIN_BUILD}/lib")
