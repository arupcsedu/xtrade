cmake_minimum_required(VERSION 3.28)

project(
  AegisMX
  VERSION 0.1.0
  DESCRIPTION "Aegis-MX deterministic engineering foundation"
  LANGUAGES CXX
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(FetchContent)
include(GNUInstallDirs)
include(ProjectOptions)

find_package(OpenSSL 1.1.1 REQUIRED COMPONENTS Crypto)

option(AEGIS_BUILD_TESTS "Build Aegis-MX C++ tests" ON)
option(AEGIS_BUILD_BENCHMARKS "Build Aegis-MX benchmark targets" ON)
option(AEGIS_BUILD_SCHEMA_COMPILER "Build the pinned FlatBuffers compiler" OFF)
option(AEGIS_BUILD_FUZZERS "Build libFuzzer deserialization targets" OFF)
option(AEGIS_WARNINGS_AS_ERRORS "Treat project warnings as errors" OFF)
option(AEGIS_ENABLE_CLANG_TIDY "Run clang-tidy while compiling project targets" OFF)
option(AEGIS_ENABLE_ASAN "Enable AddressSanitizer for project targets" OFF)
option(AEGIS_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer for project targets" OFF)
option(AEGIS_ENABLE_TSAN "Enable ThreadSanitizer for project targets" OFF)
option(AEGIS_ENABLE_CCACHE "Use ccache when it is available" ON)
option(
  AEGIS_ENABLE_LIVE_TRADING
  "Compile the live-only adapter boundary; does not enable transmission"
  OFF
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(BUILD_SHARED_LIBS OFF)

set(FLATBUFFERS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_BUILD_GRPCTEST OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_BUILD_FLATHASH OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_BUILD_SHAREDLIB OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_BUILD_FLATLIB OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_BUILD_FLATC ${AEGIS_BUILD_SCHEMA_COMPILER} CACHE BOOL "" FORCE)
set(FLATBUFFERS_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  flatbuffers
  URL https://github.com/google/flatbuffers/archive/refs/tags/v25.12.19.tar.gz
  URL_HASH SHA256=f81c3162b1046fe8b84b9a0dbdd383e24fdbcf88583b9cb6028f90d04d90696a
)
FetchContent_MakeAvailable(flatbuffers)

aegis_validate_options()
aegis_configure_ccache()
aegis_create_project_option_targets()
aegis_resolve_source_revision(AEGIS_RESOLVED_SOURCE_REVISION)

if(AEGIS_ENABLE_LIVE_TRADING)
  set(AEGIS_LIVE_TRADING_CAPABLE_LITERAL true)
else()
  set(AEGIS_LIVE_TRADING_CAPABLE_LITERAL false)
endif()

set(AEGIS_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${AEGIS_GENERATED_INCLUDE_DIR}/aegis/common")

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cpp/common/include/aegis/common/build_info_generated.hpp.in"
  "${AEGIS_GENERATED_INCLUDE_DIR}/aegis/common/build_info_generated.hpp"
  @ONLY
)
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build-metadata.json.in"
  "${CMAKE_CURRENT_BINARY_DIR}/build-metadata.json"
  @ONLY
)

if(AEGIS_BUILD_TESTS)
  enable_testing()
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
    URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
  )
  FetchContent_MakeAvailable(googletest)
endif()

if(AEGIS_BUILD_BENCHMARKS)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    googlebenchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.9.5.tar.gz
    URL_HASH SHA256=9631341c82bac4a288bef951f8b26b41f69021794184ece969f8473977eaa340
  )
  FetchContent_MakeAvailable(googlebenchmark)
endif()

add_subdirectory(cpp/common)
add_subdirectory(cpp/event_bus)
add_subdirectory(cpp/journal)
add_subdirectory(cpp/market_data)
add_subdirectory(cpp/order_book)
add_subdirectory(cpp/features)
add_subdirectory(cpp/models)
add_subdirectory(cpp/ensemble)
add_subdirectory(cpp/risk)
add_subdirectory(cpp/oms)
add_subdirectory(cpp/execution)
add_subdirectory(cpp/high_availability)
add_subdirectory(cpp/observability)
add_subdirectory(cpp/replay)
add_subdirectory(cpp/backtesting)
add_subdirectory(cpp/integration)
add_subdirectory(cpp/edge_services)

if(AEGIS_BUILD_BENCHMARKS)
  add_subdirectory(cpp/benchmarks)
endif()

install(
  FILES "${CMAKE_CURRENT_BINARY_DIR}/build-metadata.json"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/aegis-mx"
)

set(CPACK_PACKAGE_NAME "aegis-mx-foundation")
set(CPACK_PACKAGE_VENDOR "Aegis-MX")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(
  CPACK_PACKAGE_FILE_NAME
  "aegis-mx-foundation-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}"
)
set(CPACK_GENERATOR "TGZ")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
include(CPack)
