#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "harness.hpp"
#include "xsv/csv.hpp"
#include "xsv/index.hpp"

namespace {

std::filesystem::path temp_path(std::string_view name) {
  return std::filesystem::temp_directory_path() /
         ("xsv-cpp-test-" + std::string(name) + ".idx");
}

std::uint64_t read_be(const std::string& bytes, std::size_t at) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value = (value << 8) |
            static_cast<unsigned char>(bytes[at + static_cast<std::size_t>(i)]);
  }
  return value;
}

}  // namespace

TEST(index_format_is_big_endian_offsets_then_count) {
  const std::string text = "h1,h2\n1,2\n3,4\n";
  xsv::csv::ReaderOptions options;
  xsv::csv::Reader reader(std::span<const char>(text), options);
  std::ostringstream out;
  CHECK(xsv::create_index(reader, out).has_value());
  const std::string bytes = out.str();
  CHECK_EQ(bytes.size(), 4u * 8u);
  CHECK_EQ(read_be(bytes, 0), 0u);
  CHECK_EQ(read_be(bytes, 8), std::uint64_t{6});
  CHECK_EQ(read_be(bytes, 16), std::uint64_t{10});
  CHECK_EQ(read_be(bytes, 24), 3u);
}

TEST(index_bom_header_offset_is_zero) {
  const std::string text = "\xEF\xBB\xBF" "a,b\n1,2\n";
  xsv::csv::ReaderOptions options;
  xsv::csv::Reader reader(std::span<const char>(text), options);
  std::ostringstream out;
  CHECK(xsv::create_index(reader, out).has_value());
  const std::string bytes = out.str();
  CHECK_EQ(read_be(bytes, 0), 0u);
  CHECK_EQ(read_be(bytes, 8), std::uint64_t{7});
  CHECK_EQ(read_be(bytes, 16), 2u);
}

TEST(index_opens_and_resolves_offsets) {
  const std::string text = "h1,h2\n1,2\n3,4\n";
  xsv::csv::ReaderOptions options;
  xsv::csv::Reader reader(std::span<const char>(text), options);
  std::ostringstream out;
  CHECK(xsv::create_index(reader, out).has_value());

  const auto path = temp_path("offsets");
  {
    std::ofstream file(path, std::ios::binary);
    file << out.str();
  }
  auto index = xsv::Index::open(path);
  CHECK(index.has_value());
  CHECK_EQ(index->count(), 3u);
  auto first = index->offset(0);
  CHECK(first.has_value());
  CHECK_EQ(*first, 0u);
  auto third = index->offset(2);
  CHECK(third.has_value());
  CHECK_EQ(*third, 10u);
  CHECK(!index->offset(3).has_value());
  std::filesystem::remove(path);
}

TEST(index_rejects_truncated_files) {
  const auto path = temp_path("short");
  {
    std::ofstream file(path, std::ios::binary);
    file << "abc";
  }
  CHECK(!xsv::Index::open(path).has_value());
  std::filesystem::remove(path);
}
