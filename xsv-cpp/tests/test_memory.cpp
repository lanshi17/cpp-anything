#include <cstddef>
#include <memory_resource>
#include <string>
#include <vector>

#include "harness.hpp"
#include "xsv/memory.hpp"

TEST(memory_arena_counts_and_resets) {
  xsv::memory::Arena arena(1024);
  {
    std::pmr::vector<std::uint64_t> values(arena.resource());
    for (std::uint64_t i = 0; i < 1000; ++i) values.push_back(i);
    CHECK(arena.peak_bytes() >= arena.live_bytes());
    CHECK(arena.live_bytes() > 0u);
  }
  const std::size_t peak = arena.peak_bytes();
  CHECK(peak > 0u);
  arena.reset();
  CHECK(arena.peak_bytes() >= peak);
}

TEST(memory_arena_serves_strings) {
  xsv::memory::Arena arena;
  std::pmr::string first(100000, 'a', arena.resource());
  std::pmr::string second(100000, 'b', arena.resource());
  CHECK_EQ(first.size(), 100000u);
  CHECK_EQ(second.size(), 100000u);
  CHECK(arena.live_bytes() >= first.size() + second.size());
}
