# All third-party versions are pinned here (see docs/DESIGN.md §8).
# SYSTEM: our strict warning flags must not fire inside third-party headers.
include(FetchContent)

# simdjson v5.0.5 (2026-10-14): the newest release with two weeks of soak at pin time.
FetchContent_Declare(simdjson
  GIT_REPOSITORY https://github.com/simdjson/simdjson.git
  GIT_TAG        v5.0.5
  GIT_SHALLOW    TRUE
  SYSTEM)
set(SIMDJSON_DEVELOPER_MODE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(simdjson)

# zlib from the OS (macOS SDK / zlib1g-dev): reads gzip-compressed recordings.
find_package(ZLIB REQUIRED)

if(CB_BUILD_SERVICE)
  # Boost 1.92.0 release tarball, hash-pinned; only Beast (+ its Asio dependency) is
  # configured, which keeps configure time and disk use down.
  set(BOOST_INCLUDE_LIBRARIES beast CACHE STRING "" FORCE)
  set(BOOST_ENABLE_CMAKE ON CACHE BOOL "" FORCE)
  FetchContent_Declare(Boost
    URL      https://github.com/boostorg/boost/releases/download/boost-1.92.0/boost-1.92.0-cmake.tar.xz
    URL_HASH SHA256=9bed76128d4e46755dbe818487788c6fceb6f72b378f4daa49b7e1e600d9088d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    EXCLUDE_FROM_ALL
    SYSTEM)
  FetchContent_MakeAvailable(Boost)

  # OpenSSL comes from the OS so it receives security updates. Homebrew installs it
  # keg-only, so point CMake at it on macOS.
  if(APPLE AND NOT OPENSSL_ROOT_DIR)
    execute_process(COMMAND brew --prefix openssl@3 OUTPUT_VARIABLE _brew_ssl
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(_brew_ssl)
      set(OPENSSL_ROOT_DIR ${_brew_ssl})
    endif()
  endif()
  find_package(OpenSSL 3 REQUIRED)
endif()

if(CB_BUILD_TESTS)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.18.0
    GIT_SHALLOW    TRUE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

if(CB_BUILD_BENCH)
  FetchContent_Declare(benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.10.0
    GIT_SHALLOW    TRUE
    SYSTEM)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(benchmark)
endif()
