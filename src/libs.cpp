#include "cppan/libs.hpp"

#include <algorithm>

#include "cppan/json.hpp"
#include "cppan/libs_json.hpp"

namespace cppan::libs {
namespace {

std::vector<Entry> load() {
  std::vector<Entry> table;
  const std::string_view raw(data::libs_json, data::libs_json_size);
  Json doc;
  try {
    doc = parse(raw);
  } catch (const JsonError&) {
    return table;
  }
  const Json* python = doc.find("python");
  if (python == nullptr || !python->is_array()) return table;
  for (const Json& item : python->as_array()) {
    Entry entry;
    const auto strings = [&](std::string_view key) {
      std::vector<std::string> out;
      const Json* value = item.find(key);
      if (value == nullptr || !value->is_array()) return out;
      for (const Json& element : value->as_array()) {
        if (element.is_string()) out.push_back(element.as_string());
      }
      return out;
    };
    entry.names = strings("names");
    if (const Json* libs = item.find("libs"); libs != nullptr && libs->is_string()) {
      entry.libs = libs->as_string();
    }
    if (const Json* fit = item.find("fit"); fit != nullptr && fit->is_string()) {
      const std::string& label = fit->as_string();
      entry.fit = label == "good"    ? Fit::Good
                  : label == "none"  ? Fit::None
                  : label == "kept"  ? Fit::Kept
                                     : Fit::Partial;
    }
    if (const Json* note = item.find("note"); note != nullptr && note->is_string()) {
      entry.note = note->as_string();
    }
    table.push_back(std::move(entry));
  }
  return table;
}

}  // namespace

std::string_view fit_label(Fit fit) {
  switch (fit) {
    case Fit::Good: return "good";
    case Fit::Partial: return "partial";
    case Fit::None: return "none";
    case Fit::Kept: return "kept";
  }
  return "partial";
}

const Entry* find(std::string_view package) {
  static const std::vector<Entry> table = load();
  for (const Entry& entry : table) {
    for (const std::string& name : entry.names) {
      if (name == package) return &entry;
    }
  }
  return nullptr;
}

std::string_view reduce(std::string_view import) {
  while (!import.empty() && (import.front() == ' ' || import.front() == '\t' ||
                             import.front() == '\n' || import.front() == '\r')) {
    import.remove_prefix(1);
  }
  while (!import.empty() && (import.back() == ' ' || import.back() == '\t' ||
                             import.back() == '\n' || import.back() == '\r')) {
    import.remove_suffix(1);
  }
  if (import.starts_with('@')) {
    const std::string_view rest = import.substr(1);
    const std::size_t first = rest.find('/');
    if (first == std::string_view::npos) return import;
    const std::string_view scope = rest.substr(0, first);
    std::string_view name = rest.substr(first + 1);
    if (const std::size_t next = name.find('/'); next != std::string_view::npos) {
      name = name.substr(0, next);
    }
    if (name.empty() || scope.empty()) return import;
    const std::size_t length = scope.size() + 1 + name.size();
    return import.substr(0, std::min(length, import.size()));
  }
  const std::size_t dot = import.find('.');
  if (dot == std::string_view::npos) return import;
  return import.substr(0, dot);
}

}  // namespace cppan::libs
