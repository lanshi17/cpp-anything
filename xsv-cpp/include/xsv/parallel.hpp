#pragma once

#include <atomic>
#include <cstddef>
#include <thread>
#include <utility>

#include "xsv/common.hpp"

#if XSV_USE_TBB
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#endif

namespace xsv::parallel {

// Number of worker threads the next command may use. XSV_THREADS overrides the
// hardware default; 0 means "all available". The value is read once at command
// start so a benchmark's thread count is explicit and recorded.
[[nodiscard]] unsigned configured_threads() noexcept;
void set_threads(unsigned threads) noexcept;
void configure_from_environment();
// Releases the TBB global limit while the TBB runtime is still alive. The CLI
// calls this before returning; destroying global_control during static teardown
// can run after the TBB runtime has already shut down.
void shutdown() noexcept;

// Deterministic index-space parallel loop. fn(i) must not throw; commands
// return errors through their own accumulators.
template <class F>
void for_each_index(std::size_t count, std::size_t grain, F&& fn) {
  if (count == 0) return;
#if XSV_USE_TBB
  if (configured_threads() != 1 && count > grain) {
    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, count, grain),
                      [&fn](const tbb::blocked_range<std::size_t>& range) {
                        for (std::size_t i = range.begin(); i != range.end();
                             ++i) {
                          fn(i);
                        }
                      });
    return;
  }
#endif
  (void)grain;
  for (std::size_t i = 0; i < count; ++i) fn(i);
}

}  // namespace xsv::parallel
