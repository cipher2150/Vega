cmake_minimum_required(VERSION 3.25)  # FetchContent SYSTEM
project(consolidated_book VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(CB_BUILD_TESTS "Build unit tests" ON)
option(CB_BUILD_BENCH "Build microbenchmarks" ON)
option(CB_BUILD_SERVICE "Build the live service (needs Boost.Beast + OpenSSL)" ON)
option(CB_WARNINGS_AS_ERRORS "Treat warnings as errors on project targets" OFF)
set(CB_SANITIZER "" CACHE STRING "Sanitizer set: '', 'asan-ubsan', or 'tsan'")

# Linux/aarch64 toolchains default to plain armv8-a, which lacks the CRC32 instructions
# Kraken's checksum uses; every target we deploy to (Ampere Altra = Neoverse N1) has them.
# Apple arm64 targets already include them.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  add_compile_options(-march=armv8-a+crc)
endif()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(Warnings)
include(Sanitizers)
include(Dependencies)

# --- core: header-only building blocks (fixed-point, SPSC ring, book) ---------
add_library(cb_core INTERFACE)
target_include_directories(cb_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(cb_core INTERFACE cxx_std_20)

# --- platform: thread placement, logging --------------------------------------
add_library(cb_platform STATIC src/platform/thread.cpp src/core/log.cpp src/core/alloc_counter.cpp)
target_link_libraries(cb_platform PUBLIC cb_core)
find_package(Threads REQUIRED)
target_link_libraries(cb_platform PUBLIC Threads::Threads)
cb_set_warnings(cb_platform)

# --- lib: venue protocols, engine, record/replay (no networking) --------------
add_library(cb_lib STATIC
  src/venue/protocol.cpp
  src/venue/kraken.cpp
  src/venue/coinbase.cpp
  src/venue/okx.cpp
  src/venue/bybit.cpp
  src/venue/factory.cpp
  src/engine/config.cpp
  src/engine/engine.cpp
  src/engine/pipeline.cpp
  src/record/recorder.cpp
  src/record/recording.cpp
  src/record/replay.cpp
  src/publish/snapshot_json.cpp)
target_link_libraries(cb_lib PUBLIC cb_platform simdjson::simdjson ZLIB::ZLIB)
cb_set_warnings(cb_lib)

if(CB_BUILD_SERVICE)
  # Beast/Asio are the heaviest headers in the build; they are confined to these two
  # translation units so only they pay the compile-time memory cost.
  add_library(cb_net STATIC src/net/ws_session.cpp src/publish/publisher.cpp)
  target_link_libraries(cb_net PUBLIC cb_lib Boost::beast OpenSSL::SSL OpenSSL::Crypto)
  cb_set_warnings(cb_net)

  add_executable(cbook src/app/main.cpp src/app/alloc_hooks.cpp)
  target_link_libraries(cbook PRIVATE cb_net)
  cb_set_warnings(cbook)
endif()

if(CB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(CB_BUILD_BENCH)
  add_subdirectory(bench)
endif()
