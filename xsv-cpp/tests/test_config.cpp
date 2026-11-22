#include <filesystem>
#include <string>

#include "harness.hpp"
#include "xsv/config.hpp"

TEST(config_delimiter_parsing) {
  auto comma = xsv::parse_delimiter(",");
  CHECK(comma.has_value());
  CHECK_EQ(*comma, ',');
  auto tab = xsv::parse_delimiter("\\t");
  CHECK(tab.has_value());
  CHECK_EQ(*tab, '\t');
  CHECK(!xsv::parse_delimiter("ab").has_value());
  CHECK(!xsv::parse_delimiter("\xCE\xB1").has_value());
}

TEST(config_tsv_extension_selects_tab) {
  auto config = xsv::Config::from_path("data.tsv");
  CHECK(config.has_value());
  CHECK_EQ(config->delimiter, '\t');
  auto csv = xsv::Config::from_path("data.csv");
  CHECK(csv.has_value());
  CHECK_EQ(csv->delimiter, ',');
  auto stdin_config = xsv::Config::from_path(std::nullopt);
  CHECK(stdin_config.has_value());
  CHECK(stdin_config->is_stdin());
}

TEST(config_index_path_appends_idx) {
  auto config = xsv::Config::from_path("data.csv");
  CHECK(config.has_value());
  CHECK_EQ(config->index_path(), std::filesystem::path("data.csv.idx"));
}
