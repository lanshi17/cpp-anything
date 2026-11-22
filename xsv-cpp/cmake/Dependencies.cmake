# Pinned third-party sources. The rule is: a benchmark number is only
# reproducible if the exact dependency commit is recorded, so every fetch below
# names a commit SHA, not a moving tag. SHA-256 of the fetched trees is recorded
# by scripts/collect_env.py.
#
# System packages win when the exact version is installed; otherwise the pinned
# source is fetched. XSV_FETCH_DEPS=OFF makes a missing system package a
# configure-time error (used by the offline audit in CI).

include(FetchContent)

set(XSV_TBB_COMMIT    f1862f38f83568d96e814e469ab61f88336cc595) # v2022.3.0
set(XSV_RE2_COMMIT    927f5d53caf8111721e734cf24724686bb745f55) # 2025-11-05
set(XSV_ABSL_COMMIT   2065f4ded0558c6f89fee67c8e5228feb4eb960e) # 20260817.0
set(XSV_FASTFLOAT_COMMIT dbeabeeb3e6791cb96994c2a7a084c36e62ccfb2) # v8.3.3

find_package(Threads REQUIRED)

# --- fast_float (header-only) -------------------------------------------------
find_package(FastFloat CONFIG QUIET)
if(NOT FastFloat_FOUND)
  if(NOT XSV_FETCH_DEPS)
    message(FATAL_ERROR "FastFloat not installed and XSV_FETCH_DEPS=OFF")
  endif()
  FetchContent_Declare(fast_float
    GIT_REPOSITORY https://github.com/fastfloat/fast_float.git
    GIT_TAG ${XSV_FASTFLOAT_COMMIT}
    GIT_SHALLOW FALSE)
  set(FASTFLOAT_TEST OFF CACHE BOOL "" FORCE)
  set(FASTFLOAT_INSTALL OFF CACHE BOOL "" FORCE)
  set(FASTFLOAT_BENCHMARKS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(fast_float)
endif()

# --- oneTBB ------------------------------------------------------------------
if(XSV_WITH_TBB)
  find_package(TBB 2021.5 CONFIG QUIET)
  if(NOT TBB_FOUND)
    if(NOT XSV_FETCH_DEPS)
      message(FATAL_ERROR "oneTBB not installed and XSV_FETCH_DEPS=OFF")
    endif()
    set(TBB_TEST OFF CACHE BOOL "" FORCE)
    set(TBB_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(TBB_STRICT OFF CACHE BOOL "" FORCE)
    set(TBBMALLOC_BUILD OFF CACHE BOOL "" FORCE)
    set(TBB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(TBB_BUILD_STATIC ON CACHE BOOL "" FORCE)
    FetchContent_Declare(onetbb
      GIT_REPOSITORY https://github.com/oneapi-src/oneTBB.git
      GIT_TAG ${XSV_TBB_COMMIT}
      GIT_SHALLOW FALSE)
    FetchContent_MakeAvailable(onetbb)
  endif()
endif()

# --- Google Benchmark (optional, microbenchmarks only) -----------------------
if(XSV_BUILD_MICROBENCH)
  set(XSV_BENCHMARK_COMMIT a6b4a9f00b36c19af9997d68a1cf6e0b5b3d032c) # v1.10.0
  find_package(benchmark CONFIG QUIET)
  if(NOT benchmark_FOUND)
    if(NOT XSV_FETCH_DEPS)
      message(FATAL_ERROR "Google Benchmark not installed and XSV_FETCH_DEPS=OFF")
    endif()
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(google_benchmark
      GIT_REPOSITORY https://github.com/google/benchmark.git
      GIT_TAG ${XSV_BENCHMARK_COMMIT}
      GIT_SHALLOW FALSE)
    FetchContent_MakeAvailable(google_benchmark)
  endif()
endif()

# --- RE2 + Abseil ------------------------------------------------------------
if(XSV_WITH_RE2)
  find_package(re2 CONFIG QUIET)
  if(NOT re2_FOUND)
    if(NOT XSV_FETCH_DEPS)
      message(FATAL_ERROR "RE2 not installed and XSV_FETCH_DEPS=OFF")
    endif()

    # RE2 2025-11-05 requires Abseil. Provide the targets first so its
    # `find_package(absl REQUIRED)` sees the in-tree build.
    if(NOT TARGET absl::base)
      set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
      set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
      set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
      FetchContent_Declare(abseil
        GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
        GIT_TAG ${XSV_ABSL_COMMIT}
        GIT_SHALLOW FALSE)
      FetchContent_MakeAvailable(abseil)
    endif()

    set(RE2_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(RE2_TEST OFF CACHE BOOL "" FORCE)
    set(RE2_BENCHMARK OFF CACHE BOOL "" FORCE)
    set(RE2_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(re2
      GIT_REPOSITORY https://github.com/google/re2.git
      GIT_TAG ${XSV_RE2_COMMIT}
      GIT_SHALLOW FALSE)
    FetchContent_MakeAvailable(re2)
  endif()
endif()
