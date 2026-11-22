#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "harness.hpp"
#include "xsv/csv.hpp"

namespace {

using xsv::csv::Reader;
using xsv::csv::ReaderOptions;
using xsv::csv::Record;
using xsv::csv::Writer;
using xsv::csv::WriterOptions;

ReaderOptions defaults() {
  ReaderOptions options;
  return options;
}

std::vector<std::vector<std::string>> read_all(Reader& reader) {
  std::vector<std::vector<std::string>> rows;
  Record record;
  while (true) {
    auto next = reader.next(record);
    if (!next) throw test::Failure(__FILE__, __LINE__, next.error().message);
    if (!*next) break;
    std::vector<std::string> row;
    for (const std::string_view field : record.fields) {
      row.emplace_back(field);
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

}  // namespace

TEST(csv_simple_header_and_rows) {
  const std::string text = "h1,h2\n1,2\n3,4\n";
  Reader reader(std::span<const char>(text), defaults());
  CHECK(reader.prime().has_value());
  CHECK_EQ(reader.headers().size(), 2u);
  CHECK_EQ(reader.headers()[0], std::string_view("h1"));
  const auto rows = read_all(reader);
  CHECK_EQ(rows.size(), 2u);
  CHECK_EQ(rows[0][1], "2");
  CHECK_EQ(rows[1][0], "3");
}

TEST(csv_quoted_delimiters_and_newlines) {
  const std::string text =
      "\"a,b\",\"line\nbreak\"\n\"quote \"\"inside\"\"\",plain\n";
  Reader reader(std::span<const char>(text), defaults());
  CHECK(reader.prime().has_value());
  CHECK_EQ(reader.headers().size(), 2u);
  CHECK_EQ(reader.headers()[0], std::string_view("a,b"));
  CHECK_EQ(reader.headers()[1], std::string_view("line\nbreak"));
  const auto rows = read_all(reader);
  CHECK_EQ(rows.size(), 1u);
  CHECK_EQ(rows[0][0], "quote \"inside\"");
  CHECK_EQ(rows[0][1], "plain");
}

TEST(csv_crlf_and_blank_lines) {
  const std::string text = "\r\n\r\na,b\r\n1,2\r\n\r\n3,4\r\n";
  Reader reader(std::span<const char>(text), defaults());
  CHECK(reader.prime().has_value());
  CHECK_EQ(reader.headers().size(), 2u);
  const auto rows = read_all(reader);
  CHECK_EQ(rows.size(), 2u);
  CHECK_EQ(rows[0][1], "2");
  CHECK_EQ(rows[1][1], "4");
}

TEST(csv_empty_fields) {
  const std::string text = "a,b,\n,,\n";
  Reader reader(std::span<const char>(text), defaults());
  CHECK(reader.prime().has_value());
  CHECK_EQ(reader.headers().size(), 3u);
  const auto rows = read_all(reader);
  CHECK_EQ(rows.size(), 1u);
  CHECK_EQ(rows[0].size(), 3u);
  CHECK_EQ(rows[0][2], "");
}

TEST(csv_single_quoted_empty_record) {
  const std::string text = "x\n\"\"\n";
  Reader reader(std::span<const char>(text), defaults());
  CHECK(reader.prime().has_value());
  const auto rows = read_all(reader);
  CHECK_EQ(rows.size(), 1u);
  CHECK_EQ(rows[0].size(), 1u);
  CHECK_EQ(rows[0][0], "");
}

TEST(csv_bom_is_stripped) {
  const std::string text = "\xEF\xBB\xBFh1,h2\n1,2\n";
  Reader reader(std::span<const char>(text), defaults());
  CHECK(reader.prime().has_value());
  CHECK_EQ(reader.headers()[0], std::string_view("h1"));
}

TEST(csv_no_headers_counts_first_row) {
  ReaderOptions options = defaults();
  options.has_headers = false;
  const std::string text = "1,2\n3,4\n";
  Reader reader(std::span<const char>(text), options);
  const auto rows = read_all(reader);
  CHECK_EQ(rows.size(), 2u);
  CHECK_EQ(rows[0][0], "1");
}

TEST(csv_ragged_record_is_an_error) {
  const std::string text = "a,b\n1,2,3\n";
  Reader reader(std::span<const char>(text), defaults());
  CHECK(reader.prime().has_value());
  Record record;
  auto next = reader.next(record);
  CHECK(!next);
  CHECK(next.error().message.find("found record with 3 fields") !=
        std::string::npos);
}

TEST(csv_flexible_allows_ragged_records) {
  ReaderOptions options = defaults();
  options.flexible = true;
  const std::string text = "a,b\n1,2,3\n";
  Reader reader(std::span<const char>(text), options);
  CHECK(reader.prime().has_value());
  const auto rows = read_all(reader);
  CHECK_EQ(rows.size(), 1u);
  CHECK_EQ(rows[0].size(), 3u);
}

TEST(csv_unclosed_quote_is_an_error) {
  const std::string text = "a,b\n\"unterminated\n";
  Reader reader(std::span<const char>(text), defaults());
  CHECK(reader.prime().has_value());
  Record record;
  auto next = reader.next(record);
  CHECK(!next);
  CHECK(next.error().message.find("EOF inside quoted field") !=
        std::string::npos);
}

TEST(csv_empty_input_has_no_records) {
  const std::string text;
  Reader reader(std::span<const char>(text), defaults());
  CHECK(reader.prime().has_value());
  CHECK_EQ(reader.headers().size(), 0u);
  Record record;
  auto next = reader.next(record);
  CHECK(next.has_value());
  CHECK(!*next);
}

TEST(csv_byte_offsets_and_lines) {
  const std::string text = "h\n1\n2\n";
  Reader reader(std::span<const char>(text), defaults());
  CHECK(reader.prime().has_value());
  Record record;
  auto next = reader.next(record);
  CHECK(next.has_value() && *next);
  CHECK_EQ(record.byte_offset, 2u);
  CHECK_EQ(record.line, 2u);
  next = reader.next(record);
  CHECK(next.has_value() && *next);
  CHECK_EQ(record.byte_offset, 4u);
  CHECK_EQ(record.line, 3u);
}

TEST(writer_quotes_only_when_necessary) {
  std::ostringstream out;
  Writer writer(out, WriterOptions{});
  CHECK(writer.write_record(std::vector<std::string_view>{"a", "b,c"}).has_value());
  CHECK(writer.write_record(std::vector<std::string_view>{"a\"b", "plain"}).has_value());
  CHECK(writer.flush().has_value());
  CHECK_EQ(out.str(), std::string("a,\"b,c\"\n\"a\"\"b\",plain\n"));
}

TEST(writer_single_empty_field_is_quoted) {
  std::ostringstream out;
  Writer writer(out, WriterOptions{});
  CHECK(writer.write_record(std::vector<std::string_view>{""}).has_value());
  CHECK(writer.flush().has_value());
  CHECK_EQ(out.str(), std::string("\"\"\n"));
}

TEST(writer_round_trips_through_reader) {
  std::ostringstream out;
  Writer writer(out, WriterOptions{});
  const std::vector<std::vector<std::string_view>> rows = {
      {"a", "b,c", "d\"e"},
      {"line\nbreak", "", " tail "},
  };
  for (const auto& row : rows) {
    CHECK(writer.write_record(row).has_value());
  }
  CHECK(writer.flush().has_value());

  const std::string written = out.str();
  ReaderOptions options = defaults();
  options.has_headers = false;
  Reader reader(std::span<const char>(written), options);
  const auto parsed = read_all(reader);
  CHECK_EQ(parsed.size(), 2u);
  CHECK_EQ(parsed[0].size(), 3u);
  CHECK_EQ(parsed[0][1], "b,c");
  CHECK_EQ(parsed[0][2], "d\"e");
  CHECK_EQ(parsed[1][0], "line\nbreak");
}
