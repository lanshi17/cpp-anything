// cpp-anything driver for textkit.c (c -> C++).
//
// Generated: replayed by `cpp-anything diff`, timed by `cpp-anything bench`.
// The driver every generated port gets: replay recorded cases through the port,
// and time them against the original's per-call cost.
//
//   cppan_driver --cases cases.jsonl --out results.jsonl
//   cppan_driver --cases cases.jsonl --bench 2000
//   cppan_driver --list
//
// It is the only part of a port that knows about JSON. Keeping it generated
// (rather than hand-written per project) is what lets `diff` trust the numbers.
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "cppan/json.hpp"
#include "cppan/profile.hpp"
#include "cppan/wire.hpp"

void cppan_register(cppan::Registry& registry);  // generated in dispatch.cpp

namespace {

struct Options {
  std::string cases = "cases.jsonl";
  std::string out = "results.jsonl";
  std::string only;
  long bench = 0;
  bool list = false;
  bool quiet = false;
};

[[nodiscard]] Options parse_args(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto next = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "cppan_driver: " << name << " needs a value\n";
        std::exit(2);
      }
      return argv[++i];
    };
    if (arg == "--cases") {
      options.cases = next("--cases");
    } else if (arg == "--out") {
      options.out = next("--out");
    } else if (arg == "--only") {
      options.only = next("--only");
    } else if (arg == "--bench") {
      options.bench = std::atol(next("--bench").c_str());
    } else if (arg == "--list") {
      options.list = true;
    } else if (arg == "--quiet") {
      options.quiet = true;
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "usage: cppan_driver [--cases FILE] [--out FILE] [--only FN] "
                   "[--bench N] [--list]\n";
      std::exit(0);
    } else {
      std::cerr << "cppan_driver: unknown argument " << arg << "\n";
      std::exit(2);
    }
  }
  return options;
}

struct Case {
  std::string id;
  std::string fn;
  std::vector<cppan::Json> args;
  cppan::Json expect;
};

[[nodiscard]] Case read_case(const cppan::Json& line) {
  Case out;
  const cppan::Json* id = line.find("id");
  const cppan::Json* fn = line.find("fn");
  const cppan::Json* args = line.find("args");
  const cppan::Json* expect = line.find("expect");
  if (id == nullptr || fn == nullptr || args == nullptr) {
    throw cppan::JsonError("case without id, fn or args");
  }
  out.id = id->as_string();
  out.fn = fn->as_string();
  out.args = args->as_array();
  out.expect = expect != nullptr ? *expect : cppan::Json::null();
  return out;
}

// One call, with every failure shape normalised into {"ok":...} or {"err":...}.
[[nodiscard]] cppan::Json run_case(const cppan::Registry& registry, const Case& test) {
  const cppan::Fn* fn = registry.find(test.fn);
  if (fn == nullptr) {
    return cppan::error_json("UnknownFunction",
                             "no port for '" + test.fn + "' in this project");
  }
  try {
    return (*fn)(test.args);
  } catch (const cppan::PortError& error) {
    return cppan::error_json(error);
  } catch (const std::exception& error) {
    return cppan::error_json("RuntimeError", error.what());
  } catch (...) {
    return cppan::error_json("CppAbort", "the port threw a non-standard exception");
  }
}

}  // namespace

int main(int argc, char** argv) {
  const Options options = parse_args(argc, argv);

  cppan::Registry registry;
  cppan_register(registry);

  if (options.list) {
    for (const auto name : registry.names()) std::cout << name << "\n";
    return 0;
  }

  std::ifstream input(options.cases);
  if (!input) {
    std::cerr << "cppan_driver: cannot read " << options.cases << "\n";
    return 2;
  }

  std::vector<Case> cases;
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    if (line.empty() || line == "\r") continue;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    try {
      cases.push_back(read_case(cppan::parse(line)));
    } catch (const std::exception& error) {
      std::cerr << "cppan_driver: " << options.cases << ":" << line_number << ": "
                << error.what() << "\n";
      return 2;
    }
  }
  if (!options.only.empty()) {
    std::erase_if(cases, [&](const Case& test) { return test.fn != options.only; });
  }

  if (options.bench > 0) {
    if (cases.empty()) {
      std::cerr << "cppan_driver: nothing to bench\n";
      return 2;
    }
    std::size_t sink = 0;
    const auto start = std::chrono::steady_clock::now();
    for (long repeat = 0; repeat < options.bench; ++repeat) {
      for (const Case& test : cases) {
        sink += cppan::dump(run_case(registry, test)).size();
      }
    }
    const auto stop = std::chrono::steady_clock::now();
    const double nanos =
        std::chrono::duration<double, std::nano>(stop - start).count();
    const auto calls = static_cast<double>(options.bench) * static_cast<double>(cases.size());
    // The port may print (the original did): the bench number goes to stderr so a
    // caller can parse it without filtering the app's own output.
    std::cerr << "{\"calls\":" << static_cast<long long>(calls)
              << ",\"ns_per_call\":" << (nanos / calls)
              << ",\"sink\":" << sink << "}\n";
    return 0;
  }

  std::ofstream output(options.out, std::ios::trunc);
  if (!output) {
    std::cerr << "cppan_driver: cannot write " << options.out << "\n";
    return 2;
  }
  for (const Case& test : cases) {
    cppan::Json::Object entry;
    entry.emplace_back("id", cppan::Json::string(test.id));
    entry.emplace_back("result", run_case(registry, test));
    output << cppan::dump(cppan::Json::object(std::move(entry))) << '\n';
  }
  if (!options.quiet) {
    std::cerr << "cppan_driver: " << cases.size() << " cases -> " << options.out << "\n";
  }
  return 0;
}
