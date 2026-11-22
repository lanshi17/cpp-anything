cmake_minimum_required(VERSION 3.28)
project(xsv_cpp VERSION 0.1.0 LANGUAGES CXX)

# xsv-cpp is a controlled refactoring experiment: one compiler family, one
# language standard, one dependency set. Portability to other compilers is not
# a goal; reproducibility on the benchmark host is.
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  message(FATAL_ERROR
    "xsv-cpp v1.0 is evaluated with GCC only (found ${CMAKE_CXX_COMPILER_ID}). "
    "The experiment contract in docs/DEPENDENCIES.md pins GCC 15.2.0.")
endif()
if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
  message(FATAL_ERROR
    "GCC 14+ is required for std::expected and C++23 library facilities "
    "(found ${CMAKE_CXX_COMPILER_VERSION}).")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# --- experiment configuration ------------------------------------------------
# Every optimization axis is a build-time switch so the ablation matrix in
# docs/BENCHMARKS.md can be expressed as independent binaries. Defaults are the
# "reference optimized" configuration (C++-D + hash + mmap).
option(XSV_WITH_PMR         "Use std::pmr arenas for per-command working memory" ON)
option(XSV_WITH_SIMD        "Enable AVX2 scanning with runtime dispatch"         ON)
option(XSV_WITH_TBB         "Enable oneTBB parallelism"                         ON)
option(XSV_WITH_CUSTOM_HASH "Use the open-addressing hash table instead of std::unordered_*" ON)
option(XSV_WITH_MMAP        "Map input files instead of reading them into a buffer" ON)
option(XSV_WITH_RE2         "Build the RE2-backed regex engine used by search"   ON)

option(XSV_NATIVE           "Compile for the benchmark host (-march=native)"     OFF)
option(XSV_LTO              "Enable link-time optimization"                      ON)
option(XSV_FETCH_DEPS       "Fetch pinned dependencies when no system package exists" ON)
option(XSV_BUILD_TESTS      "Build the unit test suite"                          ON)
option(XSV_BUILD_MICROBENCH "Build Google Benchmark microbenchmarks"             OFF)
option(XSV_WARNINGS_AS_ERRORS "Treat compiler warnings as errors"                OFF)

set(XSV_SANITIZE "" CACHE STRING
    "Sanitizer set: '', 'address;undefined', or 'thread'")

set(XSV_VARIANT_NAME "all" CACHE STRING
    "Label recorded in --version and benchmark manifests")

set(XSV_UPSTREAM_VERSION "0.13.0" CACHE STRING
    "BurntSushi/xsv release this implementation is measured against")

include(cmake/CompilerOptions.cmake)
include(cmake/Sanitizers.cmake)
include(cmake/Dependencies.cmake)
include(cmake/Variants.cmake)

# --- library -----------------------------------------------------------------
add_library(xsv_core STATIC
  src/config.cpp
  src/mmap.cpp
  src/csv.cpp
  src/index.cpp
  src/select.cpp
  src/memory.cpp
  src/hash.cpp
  src/simd.cpp
  src/parallel.cpp
  src/format.cpp
  src/cli.cpp
  src/commands/count.cpp
  src/commands/index.cpp
  src/commands/headers.cpp
  src/commands/select.cpp
  src/commands/slice.cpp
  src/commands/search.cpp
  src/commands/sort.cpp
  src/commands/stats.cpp
  src/commands/join.cpp
)

target_include_directories(xsv_core
  PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_link_libraries(xsv_core PUBLIC Threads::Threads FastFloat::fast_float)
if(XSV_WITH_RE2)
  target_sources(xsv_core PRIVATE src/regex.cpp)
  target_link_libraries(xsv_core PUBLIC re2::re2)
endif()
if(XSV_WITH_TBB)
  target_link_libraries(xsv_core PUBLIC TBB::tbb)
endif()

target_compile_definitions(xsv_core PUBLIC
  XSV_USE_PMR=$<IF:$<BOOL:${XSV_WITH_PMR}>,1,0>
  XSV_USE_SIMD=$<IF:$<BOOL:${XSV_WITH_SIMD}>,1,0>
  XSV_USE_TBB=$<IF:$<BOOL:${XSV_WITH_TBB}>,1,0>
  XSV_USE_CUSTOM_HASH=$<IF:$<BOOL:${XSV_WITH_CUSTOM_HASH}>,1,0>
  XSV_USE_MMAP=$<IF:$<BOOL:${XSV_WITH_MMAP}>,1,0>
  XSV_USE_RE2=$<IF:$<BOOL:${XSV_WITH_RE2}>,1,0>
  XSV_VERSION="${PROJECT_VERSION}"
  XSV_UPSTREAM_VERSION="${XSV_UPSTREAM_VERSION}"
  XSV_VARIANT_NAME="${XSV_VARIANT_NAME}"
)

xsv_apply_warnings(xsv_core)
xsv_apply_optimization(xsv_core)
xsv_apply_sanitizers(xsv_core)

# --- CLI ---------------------------------------------------------------------
add_executable(xsv src/main.cpp)
target_link_libraries(xsv PRIVATE xsv_core)
xsv_apply_warnings(xsv)
xsv_apply_optimization(xsv)
xsv_apply_sanitizers(xsv)

include(GNUInstallDirs)
install(TARGETS xsv RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# --- tests -------------------------------------------------------------------
if(XSV_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(XSV_BUILD_MICROBENCH)
  add_subdirectory(bench)
endif()
