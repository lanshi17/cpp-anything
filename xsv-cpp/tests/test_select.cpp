#include <string>
#include <string_view>
#include <vector>

#include "harness.hpp"
#include "xsv/select.hpp"

namespace {

using xsv::Selection;

std::vector<std::string_view> headers() {
  static const std::vector<std::string> names = {"a", "b", "c", "d"};
  std::vector<std::string_view> out;
  for (const std::string& name : names) out.push_back(name);
  return out;
}

std::vector<std::size_t> parse(std::string_view spec, bool use_names = true) {
  auto selection = Selection::parse(spec, headers(), use_names);
  if (!selection) throw test::Failure(__FILE__, __LINE__, selection.error().message);
  return selection->indices();
}

}  // namespace

TEST(select_indices_and_ranges) {
  CHECK_EQ(parse("1,3"), (std::vector<std::size_t>{0, 2}));
  CHECK_EQ(parse("2-4"), (std::vector<std::size_t>{1, 2, 3}));
  CHECK_EQ(parse("4-2"), (std::vector<std::size_t>{3, 2, 1}));
  CHECK_EQ(parse("2-"), (std::vector<std::size_t>{1, 2, 3}));
  CHECK_EQ(parse("-2"), (std::vector<std::size_t>{0, 1}));
  CHECK_EQ(parse(""), (std::vector<std::size_t>{0, 1, 2, 3}));
  CHECK_EQ(parse("1,1,2"), (std::vector<std::size_t>{0, 0, 1}));
}

TEST(select_names_and_duplicates) {
  CHECK_EQ(parse("a,c"), (std::vector<std::size_t>{0, 2}));
  CHECK_EQ(parse("d-a"), (std::vector<std::size_t>{3, 2, 1, 0}));
  CHECK_EQ(parse("\"a\""), (std::vector<std::size_t>{0}));
}

TEST(select_invert) {
  CHECK_EQ(parse("!2"), (std::vector<std::size_t>{0, 2, 3}));
  CHECK_EQ(parse("!2-3"), (std::vector<std::size_t>{0, 3}));
  CHECK_EQ(parse("!"), (std::vector<std::size_t>{}));
}

TEST(select_duplicate_header_disambiguation) {
  const std::vector<std::string> names = {"x", "x", "x"};
  std::vector<std::string_view> views;
  for (const auto& name : names) views.push_back(name);
  auto second = Selection::parse("x[1]", views, true);
  CHECK(second.has_value());
  CHECK_EQ(second->indices(), (std::vector<std::size_t>{1}));
  auto third = Selection::parse("x[2]", views, true);
  CHECK(third.has_value());
  CHECK_EQ(third->indices(), (std::vector<std::size_t>{2}));
  CHECK(!Selection::parse("x[3]", views, true).has_value());
}

TEST(select_errors) {
  CHECK(!Selection::parse("0", headers(), true).has_value());
  CHECK(!Selection::parse("5", headers(), true).has_value());
  CHECK(!Selection::parse("nope", headers(), true).has_value());
  CHECK(!Selection::parse("a", headers(), false).has_value());
  CHECK(!Selection::parse("\"unclosed", headers(), true).has_value());
  CHECK(!Selection::parse("1;2", headers(), true).has_value());
}

TEST(select_apply_projects_fields) {
  const std::string text = "a,b,c,d\n1,2,3,4\n";
  xsv::csv::ReaderOptions options;
  xsv::csv::Reader reader(std::span<const char>(text), options);
  CHECK(reader.prime().has_value());
  xsv::csv::Record record;
  auto next = reader.next(record);
  CHECK(next.has_value() && *next);
  auto selection = Selection::parse("4,2", headers(), true);
  CHECK(selection.has_value());
  const auto fields = selection->apply(record);
  CHECK_EQ(fields.size(), 2u);
  CHECK_EQ(fields[0], std::string_view("4"));
  CHECK_EQ(fields[1], std::string_view("2"));
}
