// Generated batch API: many items cross once, the loop stays in C++.
// `bridge --batch function:param` decides what is here.
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "cppan/wire.hpp"
#include "textkit/port.hpp"

namespace {

char* copy_out(const std::string& text) {
  auto* out = static_cast<char*>(std::malloc(text.size() + 1));
  if (out == nullptr) return nullptr;
  std::memcpy(out, text.data(), text.size());
  out[text.size()] = '\0';
  return out;
}

template <typename Call>
std::vector<cppan::Json> map_items(const cppan::Json& items, Call call) {
  const auto& list = items.as_array();
  std::vector<cppan::Json> results(list.size());
  const unsigned workers = std::max(1u, std::thread::hardware_concurrency());
  const std::size_t chunk = (list.size() + workers - 1) / std::max<std::size_t>(1, workers);
  std::vector<std::thread> pool;
  for (unsigned worker = 0; worker < workers; ++worker) {
    const std::size_t start = worker * chunk;
    const std::size_t stop = std::min(list.size(), start + chunk);
    if (start >= stop) break;
    pool.emplace_back([&, start, stop] {
      for (std::size_t index = start; index < stop; ++index) {
        try {
          results[index] = call(list[index]);
        } catch (const cppan::PortError& error) {
          results[index] = cppan::error_json(error);
        } catch (const std::exception& error) {
          results[index] = cppan::error_json("RuntimeError", error.what());
        }
      }
    });
  }
  for (auto& worker : pool) worker.join();
  return results;
}

}  // namespace

