module;
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include "cppan/matrix_json.hpp"

module cppan.matrix;
import cppan.types;
import cppan.json;

import cppan.util;
import std;

namespace cppan {
namespace {

std::string string_field(const Json& item, std::string_view key) {
  const Json* value = item.find(key);
  if (value == nullptr || !value->is_string()) return {};
  return value->as_string();
}

std::optional<std::string> optional_string(const Json* value) {
  if (value == nullptr || !value->is_string()) return std::nullopt;
  return value->as_string();
}

std::string first_word(std::string_view text) {
  std::size_t begin = 0;
  while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
  std::size_t end = begin;
  while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end]))) ++end;
  return std::string(text.substr(begin, end - begin));
}

std::string lower_ascii(std::string_view text) {
  std::string out(text);
  for (char& ch : out) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return out;
}

std::uint32_t number_after_cpp(std::string_view standard) {
  std::string_view digits = standard;
  if (digits.starts_with("c++")) digits.remove_prefix(3);
  std::uint32_t value = 0;
  for (const char ch : digits) {
    if (ch < '0' || ch > '9') break;
    value = value * 10 + static_cast<std::uint32_t>(ch - '0');
  }
  return value;
}

}  // namespace

std::string_view status_label(Status status) {
  switch (status) {
    case Status::Yes: return "yes";
    case Status::Partial: return "partial";
    case Status::No: return "no";
  }
  return "no";
}

bool usable(Status status) { return status == Status::Yes; }

std::string_view kind_label(Kind kind) {
  return kind == Kind::Core ? "core" : "library";
}

const Support* Feature::toolchain(std::string_view toolchain_name) const {
  for (const auto& [key, entry] : support) {
    if (key == toolchain_name) return &entry;
  }
  return nullptr;
}

std::optional<std::string> Feature::first_release(std::string_view toolchain_name) const {
  const Support* entry = toolchain(toolchain_name);
  if (entry == nullptr || !usable(entry->status)) return std::nullopt;
  return entry->min;
}

std::uint32_t Feature::std_number() const { return number_after_cpp(standard); }

const Matrix& Matrix::get() {
  static const Matrix matrix = [] {
    try {
      return Matrix::parse(std::string_view(data::matrix_json, data::matrix_json_size));
    } catch (const std::exception& error) {
      std::fprintf(stderr,
                   "cpp-anything: embedded data/matrix.json is not readable (%s); "
                   "regenerate it with scripts/sync_matrix.py\n",
                   error.what());
      std::abort();
    }
  }();
  return matrix;
}

Matrix Matrix::parse(std::string_view raw) {
  Json doc;
  try {
    doc = cppan::parse(raw);
  } catch (const JsonError& error) {
    throw std::runtime_error(error.what());
  }
  if (!doc.is_object()) throw std::runtime_error("matrix is not an object");

  Matrix matrix;
  matrix.generated_at = string_field(doc, "generated_at");

  const Json* features = doc.find("features");
  if (features == nullptr || !features->is_array()) throw std::runtime_error("missing features");
  for (const Json& item : features->as_array()) {
    Feature feature;
    feature.id = string_field(item, "id");
    feature.name = string_field(item, "name");
    feature.kind = string_field(item, "kind") == "library" ? Kind::Library : Kind::Core;
    feature.standard = string_field(item, "std");
    feature.macro_id = string_field(item, "macro");
    feature.paper = string_field(item, "paper");
    const Json* entries = item.find("support");
    if (entries != nullptr && entries->is_object()) {
      for (const auto& [toolchain, value] : entries->as_object()) {
        Support entry;
        const std::string status = string_field(value, "status");
        entry.status = status == "yes"    ? Status::Yes
                       : status == "partial" ? Status::Partial
                                             : Status::No;
        entry.min = optional_string(value.find("min"));
        const Json* notes = value.find("notes");
        if (notes != nullptr && notes->is_array()) {
          for (const Json& note : notes->as_array()) {
            if (note.is_string()) entry.notes.push_back(note.as_string());
          }
        }
        feature.support.emplace_back(toolchain, std::move(entry));
      }
    }
    matrix.features.push_back(std::move(feature));
  }

  const Json* toolchains = doc.find("toolchains");
  if (toolchains != nullptr && toolchains->is_object()) {
    for (const auto& [id, value] : toolchains->as_object()) {
      const std::string label = string_field(value, "label");
      matrix.toolchains.emplace_back(id, label.empty() ? id : label);
    }
  }

  const Json* sources = doc.find("sources");
  if (sources != nullptr && sources->is_array()) {
    for (const Json& item : sources->as_array()) {
      Source source;
      source.title = string_field(item, "title");
      source.url = string_field(item, "url");
      const Json* revision = item.find("revid");
      if (revision != nullptr && revision->is_integer()) {
        source.revision = std::to_string(revision->as_int());
      }
      source.timestamp = optional_string(item.find("rev_timestamp"));
      matrix.sources.push_back(std::move(source));
    }
  }

  const Json* standards = doc.find("standards");
  if (standards != nullptr && standards->is_array()) {
    for (const Json& item : standards->as_array()) {
      if (const Json* standard = item.find("std"); standard != nullptr && standard->is_string()) {
        matrix.standards.push_back(standard->as_string());
      }
    }
  } else {
    matrix.standards = {"c++17", "c++20", "c++23"};
  }

  if (const Json* versions = doc.find("msvc_versions"); versions != nullptr) {
    if (const Json* titles = versions->find("titles"); titles != nullptr && titles->is_object()) {
      for (const auto& [version, title] : titles->as_object()) {
        if (title.is_string()) matrix.msvc_titles[version] = title.as_string();
      }
    }
  }

  if (const Json* license = doc.find("source_license"); license != nullptr) {
    std::string title = "cppreference";
    std::string name = "CC-BY-SA";
    if (const Json* value = license->find("title"); value != nullptr && value->is_string()) {
      title = value->as_string();
    }
    if (const Json* value = license->find("license"); value != nullptr && value->is_string()) {
      name = value->as_string();
    }
    matrix.license = title + " (" + name + ")";
  } else {
    matrix.license = "cppreference.com";
  }

  return matrix;
}

std::string Matrix::toolchain_label(std::string_view id) const {
  for (const auto& [name, label] : toolchains) {
    if (name == id) return label;
  }
  return std::string(id);
}

std::optional<std::string> Matrix::resolve_toolchain(std::string_view name) const {
  std::string_view trimmed = name;
  while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) {
    trimmed.remove_prefix(1);
  }
  while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) {
    trimmed.remove_suffix(1);
  }
  std::string normalized;
  normalized.reserve(trimmed.size());
  for (const char ch : trimmed) {
    normalized += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  for (char& ch : normalized) {
    if (ch == '_') ch = '-';
  }

  std::string alias;
  if (normalized == "g++" || normalized == "gcc" || normalized == "gnu" ||
      normalized == "libstdc++" || normalized == "gcc-libstdc++") {
    alias = "gcc";
  } else if (normalized == "clang" || normalized == "clang++" || normalized == "llvm" ||
             normalized == "libc++" || normalized == "clang-libc++") {
    alias = "clang";
  } else if (normalized == "msvc" || normalized == "msvc-stl" || normalized == "cl" ||
             normalized == "visual-c++" || normalized == "vs") {
    alias = "msvc";
  } else if (normalized == "apple-clang" || normalized == "appleclang" ||
             normalized == "xcode") {
    alias = "aclang";
  } else {
    alias = normalized;
  }
  for (const auto& [id, label] : toolchains) {
    if (id == alias) return alias;
  }
  return std::nullopt;
}

std::vector<const Feature*> Matrix::features_in(std::string_view standard) const {
  std::vector<const Feature*> out;
  for (const Feature& feature : features) {
    if (feature.standard == standard) out.push_back(&feature);
  }
  std::sort(out.begin(), out.end(), [](const Feature* a, const Feature* b) {
    if (kind_label(a->kind) != kind_label(b->kind)) {
      return kind_label(a->kind) < kind_label(b->kind);
    }
    return a->name < b->name;
  });
  return out;
}

Audit Matrix::audit(std::string_view toolchain, std::string_view version,
                    std::string_view standard) const {
  Audit audit;
  for (const Feature* feature : features_in(standard)) {
    const Support* support = feature->toolchain(toolchain);
    if (support != nullptr && support->status == Status::Yes) {
      if (!support->min.has_value() || version_at_least(version, *support->min)) {
        audit.supported.push_back(feature);
      } else {
        audit.missing.push_back(feature);
      }
    } else if (support != nullptr && support->status == Status::Partial) {
      audit.partial.push_back(feature);
    } else {
      audit.missing.push_back(feature);
    }
  }
  return audit;
}

std::optional<std::string> Matrix::first_complete_core(std::string_view toolchain,
                                                       std::string_view standard) const {
  return first_complete(toolchain, standard, Kind::Core);
}

std::optional<std::string> Matrix::first_complete_library(std::string_view toolchain,
                                                          std::string_view standard) const {
  return first_complete(toolchain, standard, Kind::Library);
}

std::optional<std::string> Matrix::first_complete(std::string_view toolchain,
                                                  std::string_view standard, Kind kind) const {
  std::vector<const Feature*> selected;
  for (const Feature* feature : features_in(standard)) {
    if (feature->kind == kind) selected.push_back(feature);
  }
  if (selected.empty()) return std::nullopt;

  std::optional<std::string> highest;
  for (const Feature* feature : selected) {
    const Support* support = feature->toolchain(toolchain);
    if (support == nullptr) return std::nullopt;
    if (support->status == Status::Yes) {
      // A recorded "yes" without a release still counts.
    } else if (support->status == Status::Partial) {
      continue;
    } else {
      return std::nullopt;
    }
    if (support->min.has_value()) {
      if (!highest.has_value() || version_cmp(*support->min, *highest) > 0) {
        highest = support->min;
      }
    }
  }
  return highest.value_or("any");
}

std::tuple<std::size_t, std::size_t, std::size_t> Matrix::supported_counts(
    std::string_view toolchain, std::string_view version, std::string_view standard) const {
  const Audit audit = this->audit(toolchain, version, standard);
  return {audit.supported.size(), audit.partial.size(), audit.missing.size()};
}

std::vector<const Feature*> Matrix::lookup(std::string_view query) const {
  std::string needle(query);
  // Rust trims Unicode whitespace; ASCII covers the CLI's input.
  while (!needle.empty() && std::isspace(static_cast<unsigned char>(needle.front()))) {
    needle.erase(needle.begin());
  }
  while (!needle.empty() && std::isspace(static_cast<unsigned char>(needle.back()))) {
    needle.pop_back();
  }
  if (needle.empty()) return {};

  const std::string lowered = lower_ascii(needle);
  std::vector<const Feature*> exact;
  std::vector<const Feature*> partial;
  for (const Feature& feature : features) {
    const std::string macro_name = first_word(feature.macro_id);
    if (feature.id == needle || macro_name == needle) {
      exact.push_back(&feature);
    } else if (lower_ascii(feature.name).find(lowered) != std::string::npos ||
               lower_ascii(feature.id).find(lowered) != std::string::npos ||
               macro_name.find(needle) != std::string::npos) {
      partial.push_back(&feature);
    }
  }
  exact.insert(exact.end(), partial.begin(), partial.end());
  return exact;
}

const Feature* Matrix::by_macro(std::string_view macro_name) const {
  const Feature* best = nullptr;
  for (const Feature& feature : features) {
    if (first_word(feature.macro_id) != macro_name) continue;
    if (best == nullptr || feature.std_number() < best->std_number()) best = &feature;
  }
  return best;
}

MacroStatus Matrix::macro_available(std::string_view macro_name, std::string_view toolchain,
                                    std::string_view version, std::string_view standard) const {
  const Feature* feature = by_macro(macro_name);
  if (feature == nullptr) return {};
  const std::uint32_t limit = number_after_cpp(standard);
  if (feature->std_number() > limit) {
    return {MacroStatusKind::TooNew, feature->standard, feature->name};
  }
  const Support* support = feature->toolchain(toolchain);
  if (support != nullptr && support->status == Status::Yes) {
    if (support->min.has_value()) {
      if (version_at_least(version, *support->min)) {
        return {MacroStatusKind::Available, *support->min, feature->name};
      }
      return {MacroStatusKind::TooOld, *support->min, feature->name};
    }
    return {MacroStatusKind::Available, "?", feature->name};
  }
  if (support != nullptr && support->status == Status::Partial) {
    return {MacroStatusKind::Partial, support->min.value_or("?"), feature->name};
  }
  return {MacroStatusKind::Missing, "", feature->name};
}

std::string render_profile(std::string_view toolchain, std::string_view version,
                           std::string_view standard) {
  const Matrix& matrix = Matrix::get();
  std::string out;
  out += "// Generated by `cpp-anything matrix profile`; do not edit.\n";
  out += "//\n";
  out += "// Claims about the target toolchain come from data/matrix.json, which is\n";
  out += "// synced from cppreference's compiler support tables. Regenerate with:\n";
  out += "//   cpp-anything matrix profile --toolchain ";
  out += toolchain;
  out += " --version ";
  out += version;
  out += " --std ";
  out += standard;
  out += "\n#pragma once\n\n";
  out += "#define CPPAN_TOOLCHAIN \"";
  out += toolchain;
  out += "\"\n";
  out += "#define CPPAN_TOOLCHAIN_VERSION \"";
  out += version;
  out += "\"\n";
  const std::string_view number = standard.starts_with("c++") ? standard.substr(3) : standard;
  out += "#define CPPAN_CXX_STANDARD ";
  out += number;
  out += "\n";
  out += "#define CPPAN_MATRIX_GENERATED_AT \"";
  out += matrix.generated_at;
  out += "\"\n\n";

  for (const ProfileFeature& entry : kProfileFeatures) {
    const MacroStatus status = matrix.macro_available(entry.macro_id, toolchain, version, standard);
    out += "#define ";
    out += entry.define;
    out += status.available() ? " 1  // " : " 0  // ";
    switch (status.kind) {
      case MacroStatusKind::Available:
        out += std::format("{} since {}", status.feature, status.release);
        break;
      case MacroStatusKind::TooOld:
        out += std::format("{} needs {}", status.feature, status.release);
        break;
      case MacroStatusKind::Partial:
        out += std::format("{} partial since {}", status.feature, status.release);
        break;
      case MacroStatusKind::TooNew:
        out += std::format("{} arrives in {}", status.feature, status.release);
        break;
      case MacroStatusKind::Missing:
        out += std::format("{}: not implemented", status.feature);
        break;
      case MacroStatusKind::Unknown:
        out += "no matrix entry";
        break;
    }
    out += " (";
    out += entry.macro_id;
    out += ")\n";
  }

  out += std::format(
      "\nstatic_assert(__cplusplus >= {}L,\n              \"cpp-anything needs {} for this "
      "project; see docs/guides/compilers/{} if your compiler disagrees\");\n",
      std_macro(standard), standard, toolchain);
  out +=
      "\n// std::expected on C++23 targets; cppan::Result (same shape) where the library\n"
      "// does not have it yet. The port says cppan::Result<T> either way.\n";
  return out;
}

std::string_view std_macro(std::string_view standard) {
  std::string_view number = standard;
  if (number.starts_with("c++")) number.remove_prefix(3);
  if (number == "11") return "201103";
  if (number == "14") return "201402";
  if (number == "17") return "201703";
  if (number == "20") return "202002";
  return "202302";
}

}  // namespace cppan
