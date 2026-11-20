// `profile`: run a real workload under the language's profiler and say what is
// worth moving, with the advice an agent would otherwise guess.
//
// Python keeps using the bundled probe (cProfile), which already speaks this
// module's JSON. Node, Ruby, Go and Java each get a collector here: Node's
// `--cpu-prof`, a `TracePoint` tracer for Ruby, `go test -cpuprofile` read with
// `go tool pprof -raw`, and Flight Recorder for Java. Every collector ends in
// the same ranked `Report`, so the rest of the tool does not care where the
// numbers came from.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "cppan/cli.hpp"
#include "cppan/json.hpp"
#include "cppan/util.hpp"

namespace cppan::profile {

struct Entry {
  bool harness = false;
  std::string function;
  std::string file;
  std::int64_t line = 0;
  std::int64_t calls = 0;
  double self_seconds = 0.0;
  double cum_seconds = 0.0;
  double per_call_ns = 0.0;
  double share = 0.0;
  std::vector<std::string> callers;
  std::string advice;
  bool external = false;
};

struct Report {
  std::vector<std::string> workload;
  double total_seconds = 0.0;
  std::vector<Entry> entries;
};

/// Runs the workload through the profiler that fits its runtime and turns the
/// raw stats into a ranked report. `python3 bench.py` and `bench.py` both work;
/// the probe strips the interpreter so cProfile runs the script itself. Node,
/// Ruby, Go and Java are profiled in-process and read back here.
[[nodiscard]] Result<Report> run(const std::filesystem::path& root,
                                 const std::vector<std::string>& command, std::size_t top);

[[nodiscard]] std::string render(const Report& report);
[[nodiscard]] Json to_json(const Report& report, const std::filesystem::path& root);

/// `cli.rs`'s `profile_command`: parses the report options, runs `run`, and
/// prints the text or JSON the reference prints.
[[nodiscard]] Result<int> command(const cli::Parsed& args);

}  // namespace cppan::profile
