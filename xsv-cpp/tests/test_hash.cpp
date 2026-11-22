#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "harness.hpp"
#include "xsv/hash.hpp"

namespace {

template <class Map>
void check_map() {
  Map map;
  map.reserve(1024);
  std::vector<std::string> keys;
  keys.reserve(1000);
  for (std::uint32_t i = 0; i < 1000; ++i) {
    keys.push_back("key-" + std::to_string(i));
  }
  for (std::uint32_t i = 0; i < 5000; ++i) {
    map.insert(keys[i % 1000], i);
  }
  CHECK_EQ(map.size(), 5000u);
  CHECK_EQ(map.key_count(), 1000u);

  std::vector<std::uint32_t> values;
  map.for_each_match("key-0", [&](std::uint32_t value) {
    values.push_back(value);
  });
  CHECK_EQ(values.size(), 5u);
  CHECK_EQ(values[0], 0u);
  CHECK_EQ(values[4], 4000u);

  values.clear();
  map.for_each_match("missing", [&](std::uint32_t value) {
    values.push_back(value);
  });
  CHECK_EQ(values.size(), 0u);
}

}  // namespace

TEST(hash_flat_multimap) {
  check_map<xsv::hash::FlatHashMultiMap<std::uint32_t>>();
}

TEST(hash_std_multimap) {
  check_map<xsv::hash::StdHashMultiMap<std::uint32_t>>();
}

TEST(hash_selected_multimap_matches_contract) {
  check_map<xsv::hash::MultiMap<std::uint32_t>>();
}

TEST(hash_bytes_is_deterministic_and_sensitive) {
  const std::uint64_t a = xsv::hash::hash_bytes("hello");
  const std::uint64_t b = xsv::hash::hash_bytes("hello");
  const std::uint64_t c = xsv::hash::hash_bytes("hellp");
  CHECK_EQ(a, b);
  CHECK(a != c);
}
