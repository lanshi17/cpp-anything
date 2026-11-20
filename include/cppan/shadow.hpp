// `shadow`: the port running beside the original on real traffic.
//
// The generated `shadow/<unit>.shadow.py` calls the original first and returns its
// result; the C++ port runs second, and every disagreement is appended to a log.
// Nothing the port does can reach a caller, so shipping shadow code cannot change
// behaviour — that is the property that makes it safe to run in production.
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "cppan/json.hpp"
#include "cppan/project.hpp"
#include "cppan/util.hpp"

namespace cppan::shadow {

struct Mismatch {
  std::string function;
  Json args;
  Json expected;
  Json got;
  std::string reason;
  std::string at;
};

struct Report {
  std::size_t lines = 0;
  std::vector<Mismatch> mismatches;
  std::vector<std::pair<std::string, std::size_t>> by_function;

  [[nodiscard]] bool clean() const { return mismatches.empty(); }
};

[[nodiscard]] std::filesystem::path log_path(const Project& project);
[[nodiscard]] Result<Report> read_report(const Project& project);
[[nodiscard]] std::string render(const Report& report, const Project& project);
[[nodiscard]] Json to_json(const Report& report, const Project& project);

}  // namespace cppan::shadow
