#include <cstdio>
import cppan.cli;
import std;

// cpp-anything: rewrite your app in C++ (C++20/23), and prove it still behaves
// the same.
//
// The loop is `analyze` → `bridge` → `record` → (write C++) → `build` → `diff` →
// `gate`, with `matrix` answering what the target toolchain can compile and `rsi`
// using the same loop to check cpp-anything itself.


int main(int argc, char** argv) {
  // Rust's stdout is line buffered; matching that keeps stdout/stderr interleaved
  // the same way when both are redirected to one file.
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  std::vector<std::string> args;
  args.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
  for (int index = 1; index < argc; ++index) args.emplace_back(argv[index]);

  auto result = cppan::cli::run(args);
  if (!result) {
    std::print(stderr, "cpp-anything: {}\n", result.error());
    return 1;
  }
  return *result;
}
