// perf_event_paranoid=4 blocks hardware counters on this host. This helper
// records the portable substitutes: wall time, TSC cycles, user/system CPU
// time and the wait4 rusage counters. It is not an IPC/instructions claim.
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>
#include <vector>

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

namespace {

double seconds(const struct timeval& value) {
  return static_cast<double>(value.tv_sec) +
         static_cast<double>(value.tv_usec) / 1e6;
}

std::uint64_t cycles_now() {
#if defined(__x86_64__) || defined(__i386__)
  return __rdtsc();
#else
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<std::uint64_t>(ts.tv_nsec);
#endif
}

void usage() {
  std::cerr << "usage: cpu-metrics -- <command> [args...]\n";
}

}  // namespace

int main(int argc, char** argv) {
  int separator = 1;
  while (separator < argc && std::strcmp(argv[separator], "--") != 0) {
    ++separator;
  }
  if (separator >= argc - 1) {
    usage();
    return 2;
  }
  std::vector<char*> command;
  for (int i = separator + 1; i < argc; ++i) command.push_back(argv[i]);
  command.push_back(nullptr);

  struct timespec wall_start {};
  struct timespec wall_end {};
  clock_gettime(CLOCK_MONOTONIC, &wall_start);
  const std::uint64_t start_cycles = cycles_now();

  const pid_t pid = fork();
  if (pid < 0) {
    std::perror("fork");
    return 2;
  }
  if (pid == 0) {
    const int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }
    execvp(command[0], command.data());
    _exit(127);
  }

  int status = 0;
  struct rusage usage {};
  if (wait4(pid, &status, 0, &usage) < 0) {
    std::perror("wait4");
    return 2;
  }
  const std::uint64_t end_cycles = cycles_now();
  clock_gettime(CLOCK_MONOTONIC, &wall_end);

  const double wall = static_cast<double>(wall_end.tv_sec - wall_start.tv_sec) +
                      static_cast<double>(wall_end.tv_nsec - wall_start.tv_nsec) / 1e9;
  const double user = seconds(usage.ru_utime);
  const double system = seconds(usage.ru_stime);
  const std::uint64_t cycles = end_cycles - start_cycles;
  const double effective_ghz = wall > 0 ? static_cast<double>(cycles) / wall / 1e9 : 0.0;
  const int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status);

  std::cout << "{"
            << "\"command\":\"" << command[0] << "\","
            << "\"exit\":" << exit_code << ","
            << "\"wall_s\":" << wall << ","
            << "\"user_s\":" << user << ","
            << "\"sys_s\":" << system << ","
            << "\"cpu_s\":" << (user + system) << ","
            << "\"tsc_cycles\":" << cycles << ","
            << "\"effective_ghz\":" << effective_ghz << ","
            << "\"maxrss_kb\":" << usage.ru_maxrss << ","
            << "\"minflt\":" << usage.ru_minflt << ","
            << "\"majflt\":" << usage.ru_majflt << ","
            << "\"nvcsw\":" << usage.ru_nvcsw << ","
            << "\"nivcsw\":" << usage.ru_nivcsw << ","
            << "\"inblock\":" << usage.ru_inblock << ","
            << "\"oublock\":" << usage.ru_oublock
            << "}\n";
  return exit_code == 0 ? 0 : 1;
}
