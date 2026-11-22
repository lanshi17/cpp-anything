#include "xsv/cli.hpp"

#include <csignal>
#include <iostream>
#include <string_view>
#include <vector>

#include "xsv/commands.hpp"
#include "xsv/parallel.hpp"

namespace xsv::cli {
namespace {

constexpr std::string_view kCommandList = R"(
    count       Count records
    headers     Show header names
    index       Create CSV index for faster access
    search      Search CSV data with regexes
    select      Select columns from CSV
    slice       Slice records from CSV
    sort        Sort CSV data
    stats       Compute basic statistics
    join        Join CSV files
)";

constexpr std::string_view kUsage = R"(xsv is a suite of CSV command line utilities.

Usage:
    xsv <command> [<args>...]
    xsv [options]

Options:
    --list        List all commands available.
    -h, --help    Display this message
    <command> -h  Display the command help message
    --version     Print version info and exit

Commands:
    count       Count records
    headers     Show header names
    index       Create CSV index for faster access
    search      Search CSV data with regexes
    select      Select columns from CSV
    slice       Slice records from CSV
    sort        Sort CSV data
    stats       Compute basic statistics
    join        Join CSV files
)";

const Command kCommands[] = {
    {"count", "Count records", &commands::count},
    {"headers", "Show header names", &commands::headers},
    {"index", "Create CSV index for faster access", &commands::index},
    {"search", "Search CSV data with regexes", &commands::search},
    {"select", "Select columns from CSV", &commands::select},
    {"slice", "Slice records from CSV", &commands::slice},
    {"sort", "Sort CSV data", &commands::sort},
    {"stats", "Compute basic statistics", &commands::stats},
    {"join", "Join CSV files", &commands::join},
};

}  // namespace

std::span<const Command> commands() { return kCommands; }

const Command* find_command(std::string_view name) {
  for (const Command& command : kCommands) {
    if (command.name == name) return &command;
  }
  return nullptr;
}

int main(int argc, char** argv) {
  struct ParallelShutdown {
    ~ParallelShutdown() { parallel::shutdown(); }
  } parallel_shutdown;
  // Rust's runtime ignores SIGPIPE; do the same so a closed downstream pipe
  // becomes a write error rather than a signal exit.
  std::signal(SIGPIPE, SIG_IGN);
  parallel::configure_from_environment();

  if (argc < 2) {
    std::cerr << kUsage;
    return 0;
  }

  const std::string_view first(argv[1]);
  if (first == "--version") {
    std::cout << "xsv-cpp " << XSV_VERSION << " (xsv " << XSV_UPSTREAM_VERSION
              << " compatible, variant " << XSV_VARIANT_NAME << ")\n";
    return 0;
  }
  if (first == "--list") {
    std::cout << "Installed commands:\n" << kCommandList;
    return 0;
  }
  if (first == "-h" || first == "--help") {
    std::cout << kUsage;
    return 0;
  }
  if (first == "help") {
    if (argc >= 3) {
      if (const Command* command = find_command(argv[2]); command != nullptr) {
        const std::string_view args[] = {"--help"};
        Status status = command->run(std::span<const std::string_view>(args, 1));
        if (!status) {
          std::cerr << status.error().message << '\n';
          return status.error().exit_code;
        }
        return 0;
      }
    }
    std::cout << kUsage;
    return 0;
  }

  const Command* command = find_command(first);
  if (command == nullptr) {
    std::cerr << "Unknown command '" << first << "'.\n";
    return 1;
  }

  std::vector<std::string_view> args;
  args.reserve(static_cast<std::size_t>(argc) - 2);
  for (int i = 2; i < argc; ++i) args.emplace_back(argv[i]);

  Status status = command->run(args);
  if (!status) {
    std::cerr << status.error().message << '\n';
    return status.error().exit_code;
  }
  return 0;
}

}  // namespace xsv::cli
