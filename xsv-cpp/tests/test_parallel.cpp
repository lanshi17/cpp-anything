#include <atomic>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <vector>

#include "harness.hpp"
#include "xsv/parallel.hpp"

TEST(parallel_sum_is_deterministic) {
  constexpr std::size_t count = 100000;
  std::vector<std::uint64_t> values(count, 1);
  xsv::parallel::set_threads(1);
  std::uint64_t serial = 0;
  xsv::parallel::for_each_index(count, 1024, [&](std::size_t i) {
    serial += values[i];
  });
  xsv::parallel::set_threads(4);
  std::atomic<std::uint64_t> parallel{0};
  xsv::parallel::for_each_index(count, 1024, [&](std::size_t i) {
    parallel.fetch_add(values[i], std::memory_order_relaxed);
  });
  CHECK_EQ(serial, static_cast<std::uint64_t>(count));
  CHECK_EQ(parallel.load(), static_cast<std::uint64_t>(count));
  xsv::parallel::set_threads(0);
  CHECK(xsv::parallel::configured_threads() >= 1u);
}

TEST(parallel_handles_empty_and_small_ranges) {
  xsv::parallel::set_threads(4);
  int calls = 0;
  xsv::parallel::for_each_index(0, 16, [&](std::size_t) { ++calls; });
  CHECK_EQ(calls, 0);
  xsv::parallel::for_each_index(3, 16, [&](std::size_t) { ++calls; });
  CHECK_EQ(calls, 3);
  xsv::parallel::set_threads(0);
}
