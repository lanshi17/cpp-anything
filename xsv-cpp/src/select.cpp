#include "xsv/select.hpp"

#include <charconv>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xsv {
namespace {

struct OneSelector {
  enum class Kind { kStart, kEnd, kIndex, kName };
  Kind kind = Kind::kIndex;
  std::size_t index = 0;
  std::string name;
  std::size_t name_index = 0;
};

struct Selector {
  OneSelector first;
  bool is_range = false;
  OneSelector last;
};

class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  Result<std::vector<Selector>> parse() {
    std::vector<Selector> selectors;
    while (pos_ < text_.size()) {
      OneSelector first;
      if (peek() == '-') {
        first.kind = OneSelector::Kind::kStart;
      } else {
        auto parsed = parse_one();
        if (!parsed) return std::unexpected(parsed.error());
        first = std::move(*parsed);
      }

      Selector selector;
      selector.first = std::move(first);
      if (peek() == '-') {
        ++pos_;
        selector.is_range = true;
        if (end_of_selector()) {
          selector.last.kind = OneSelector::Kind::kEnd;
        } else {
          auto parsed = parse_one();
          if (!parsed) return std::unexpected(parsed.error());
          selector.last = std::move(*parsed);
        }
      }
      if (!end_of_selector()) {
        return fail("Expected end of field but got '" +
                    std::string(1, peek()) + "' instead.");
      }
      selectors.push_back(std::move(selector));
      if (peek() == ',') ++pos_;
    }
    return selectors;
  }

 private:
  Result<OneSelector> parse_one() {
    std::string name;
    if (peek() == '"') {
      ++pos_;
      bool closed = false;
      while (pos_ < text_.size()) {
        const char c = text_[pos_];
        if (c == '"') {
          ++pos_;
          if (peek() == '"') {
            ++pos_;
            name.push_back('"');
            name.push_back('"');
            continue;
          }
          closed = true;
          break;
        }
        name.push_back(c);
        ++pos_;
      }
      if (!closed) return fail("Unclosed quote, missing closing \".");
    } else {
      while (pos_ < text_.size() && peek() != ',' && peek() != '-' &&
             peek() != '[') {
        name.push_back(peek());
        ++pos_;
      }
    }

    OneSelector selector;
    if (peek() == '[') {
      ++pos_;
      std::string digits;
      while (pos_ < text_.size() && peek() != ']') {
        digits.push_back(peek());
        ++pos_;
      }
      if (pos_ >= text_.size()) {
        return fail("Unclosed index bracket, missing closing ].");
      }
      ++pos_;
      const auto parsed = parse_usize(digits);
      if (!parsed) {
        return fail("Could not convert '" + digits + "' to an integer: " +
                    parsed.error().message);
      }
      selector.kind = OneSelector::Kind::kName;
      selector.name = std::move(name);
      selector.name_index = *parsed;
      return selector;
    }

    const auto numeric = parse_usize(name);
    if (numeric) {
      selector.kind = OneSelector::Kind::kIndex;
      selector.index = *numeric;
    } else {
      selector.kind = OneSelector::Kind::kName;
      selector.name = std::move(name);
      selector.name_index = 0;
    }
    return selector;
  }

  static Result<std::size_t> parse_usize(std::string_view text) {
    if (text.empty()) return fail("empty");
    std::size_t value = 0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
      return fail("invalid digit found in string");
    }
    return value;
  }

  [[nodiscard]] char peek() const {
    return pos_ < text_.size() ? text_[pos_] : '\0';
  }
  [[nodiscard]] bool end_of_selector() const {
    return pos_ >= text_.size() || text_[pos_] == ',';
  }

  std::string_view text_;
  std::size_t pos_ = 0;
};

Result<std::size_t> resolve(const OneSelector& selector,
                            std::span<const std::string_view> headers,
                            bool use_names) {
  switch (selector.kind) {
    case OneSelector::Kind::kStart:
      return std::size_t{0};
    case OneSelector::Kind::kEnd:
      return headers.empty() ? 0 : headers.size() - 1;
    case OneSelector::Kind::kIndex:
      if (selector.index < 1 || selector.index > headers.size()) {
        return fail("Selector index " + std::to_string(selector.index) +
                    " is out of bounds. Index must be >= 1 and <= " +
                    std::to_string(headers.size()) + ".");
      }
      return selector.index - 1;
    case OneSelector::Kind::kName: {
      if (!use_names) {
        return fail("Cannot use names ('" + selector.name +
                    "') in selection with --no-headers set.");
      }
      std::size_t found = 0;
      for (std::size_t i = 0; i < headers.size(); ++i) {
        if (headers[i] == selector.name) {
          if (found == selector.name_index) return i;
          ++found;
        }
      }
      if (found == 0) {
        return fail("Selector name '" + selector.name +
                    "' does not exist as a named header in the given CSV "
                    "data.");
      }
      return fail("Selector index '" + std::to_string(selector.name_index) +
                  "' for name '" + selector.name +
                  "' is out of bounds. Must be >= 0 and <= " +
                  std::to_string(found - 1) + ".");
    }
  }
  return fail("invalid selector");
}

}  // namespace

Result<Selection> Selection::parse(std::string_view spec,
                                   std::span<const std::string_view> headers,
                                   bool use_names) {
  bool invert = false;
  if (!spec.empty() && spec.front() == '!') {
    invert = true;
    spec.remove_prefix(1);
  }

  Parser parser(spec);
  auto selectors = parser.parse();
  if (!selectors) return std::unexpected(selectors.error());

  std::vector<std::size_t> indices;
  if (selectors->empty()) {
    if (!invert) {
      indices.reserve(headers.size());
      for (std::size_t i = 0; i < headers.size(); ++i) indices.push_back(i);
    }
    return Selection(std::move(indices));
  }

  for (const Selector& selector : *selectors) {
    auto first = resolve(selector.first, headers, use_names);
    if (!first) return std::unexpected(first.error());
    if (!selector.is_range) {
      indices.push_back(*first);
      continue;
    }
    auto last = resolve(selector.last, headers, use_names);
    if (!last) return std::unexpected(last.error());
    if (*first <= *last) {
      for (std::size_t i = *first; i <= *last; ++i) indices.push_back(i);
    } else {
      for (std::size_t i = *first + 1; i > *last; --i) indices.push_back(i - 1);
    }
  }

  if (invert) {
    std::vector<bool> selected(headers.size(), false);
    for (const std::size_t index : indices) {
      if (index < selected.size()) selected[index] = true;
    }
    std::vector<std::size_t> inverted;
    for (std::size_t i = 0; i < selected.size(); ++i) {
      if (!selected[i]) inverted.push_back(i);
    }
    return Selection(std::move(inverted));
  }
  return Selection(std::move(indices));
}

std::vector<std::string_view> Selection::apply(const csv::Record& record) const {
  std::vector<std::string_view> fields;
  fields.reserve(indices_.size());
  for (const std::size_t index : indices_) {
    fields.push_back(record.fields[index]);
  }
  return fields;
}

}  // namespace xsv
