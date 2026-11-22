#include "xsv/parallel.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdlib>
#include <memory>
#include <string_view>

#if XSV_USE_TBB
#include <tbb/global_control.h>
#endif

namespace xsv::parallel {
namespace {

std::atomic<unsigned> g_threads{0};

#if XSV_USE_TBB
std::unique_ptr<tbb::global_control> g_limit;
#endif

}  // namespace

unsigned configured_threads() noexcept {
  const unsigned requested = g_threads.load(std::memory_order_relaxed);
  if (requested != 0) return requested;
  const unsigned hardware = std::thread::hardware_concurrency();
  return hardware == 0 ? 1 : hardware;
}

void set_threads(unsigned threads) noexcept {
  g_threads.store(threads, std::memory_order_relaxed);
#if XSV_USE_TBB
  if (threads <= 1) {
    g_limit.reset();
  } else {
    g_limit = std::make_unique<tbb::global_control>(
        tbb::global_control::max_allowed_parallelism, threads);
  }
#endif
}

void shutdown() noexcept {
#if XSV_USE_TBB
  g_limit.reset();
#endif
}

void configure_from_environment() {
  const char* value = std::getenv("XSV_THREADS");
  if (value == nullptr) return;
  unsigned threads = 0;
  const std::string_view text(value);
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), threads);
  if (error != std::errc{} || end != text.data() + text.size()) return;
  set_threads(threads);
}

}  // namespace xsv::parallel
