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
#include "textkit_words/port.hpp"

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

// slug(slug(text: str) -> str) over `text`: many item(s) per call.
struct Handle_slug_text {
  cppan::Json items;
};

extern "C" {
CPPAN_EXPORT void* cppan_batch_slug_text_open(const char* items_json) {
  try {
    auto doc = cppan::parse(items_json != nullptr ? items_json : "[]");
    if (!doc.is_array()) return nullptr;
    return new Handle_slug_text{std::move(doc)};
  } catch (...) {
    return nullptr;
  }
}

CPPAN_EXPORT const char* cppan_batch_slug_text_run(void* handle, const char* args_json) {
  auto* state = static_cast<Handle_slug_text*>(handle);
  if (state == nullptr) return copy_out(cppan::dump(cppan::error_json("TypeError", "closed batch")));
  try {
    const auto args = cppan::parse(args_json != nullptr ? args_json : "[]");
    auto results = map_items(state->items, [&](const cppan::Json& item) {
      const auto value = cppan::decode<std::string_view>(item, "slug", 0);
      return cppan::encode_result(textkit_words::slug(value));
    });
    cppan::Json::Object out;
    out.emplace_back("ok", cppan::Json::array(std::move(results)));
    return copy_out(cppan::dump(cppan::Json::object(std::move(out))));
  } catch (const cppan::PortError& error) {
    return copy_out(cppan::dump(cppan::error_json(error)));
  } catch (const std::exception& error) {
    return copy_out(cppan::dump(cppan::error_json("RuntimeError", error.what())));
  }
}

CPPAN_EXPORT const char* cppan_batch_slug_text_rank(void* handle, const char* args_json, double minimum) {
  auto* state = static_cast<Handle_slug_text*>(handle);
  if (state == nullptr) return copy_out(cppan::dump(cppan::error_json("TypeError", "closed batch")));
  try {
    const auto args = cppan::parse(args_json != nullptr ? args_json : "[]");
    (void)minimum;
    return copy_out(cppan::dump(cppan::error_json("TypeError", "rank needs numeric results")));
  } catch (...) {
    return copy_out(cppan::dump(cppan::error_json("RuntimeError", "rank failed")));
  }
}

CPPAN_EXPORT void cppan_batch_slug_text_close(void* handle) {
  delete static_cast<Handle_slug_text*>(handle);
}

}  // extern "C"

// word_count(word_count(text: str) -> int) over `text`: many item(s) per call.
struct Handle_word_count_text {
  cppan::Json items;
};

extern "C" {
CPPAN_EXPORT void* cppan_batch_word_count_text_open(const char* items_json) {
  try {
    auto doc = cppan::parse(items_json != nullptr ? items_json : "[]");
    if (!doc.is_array()) return nullptr;
    return new Handle_word_count_text{std::move(doc)};
  } catch (...) {
    return nullptr;
  }
}

CPPAN_EXPORT const char* cppan_batch_word_count_text_run(void* handle, const char* args_json) {
  auto* state = static_cast<Handle_word_count_text*>(handle);
  if (state == nullptr) return copy_out(cppan::dump(cppan::error_json("TypeError", "closed batch")));
  try {
    const auto args = cppan::parse(args_json != nullptr ? args_json : "[]");
    auto results = map_items(state->items, [&](const cppan::Json& item) {
      const auto value = cppan::decode<std::string_view>(item, "word_count", 0);
      return cppan::encode_result(textkit_words::word_count(value));
    });
    cppan::Json::Object out;
    out.emplace_back("ok", cppan::Json::array(std::move(results)));
    return copy_out(cppan::dump(cppan::Json::object(std::move(out))));
  } catch (const cppan::PortError& error) {
    return copy_out(cppan::dump(cppan::error_json(error)));
  } catch (const std::exception& error) {
    return copy_out(cppan::dump(cppan::error_json("RuntimeError", error.what())));
  }
}

CPPAN_EXPORT const char* cppan_batch_word_count_text_rank(void* handle, const char* args_json, double minimum) {
  auto* state = static_cast<Handle_word_count_text*>(handle);
  if (state == nullptr) return copy_out(cppan::dump(cppan::error_json("TypeError", "closed batch")));
  try {
    const auto args = cppan::parse(args_json != nullptr ? args_json : "[]");
    auto results = map_items(state->items, [&](const cppan::Json& item) {
      const auto value = cppan::decode<std::string_view>(item, "word_count", 0);
      return cppan::encode_result(textkit_words::word_count(value));
    });
    std::vector<std::pair<double, std::size_t>> scored;
    scored.reserve(results.size());
    for (std::size_t index = 0; index < results.size(); ++index) {
      const auto& result = results[index];
      if (const cppan::Json* error = result.find("err"); error != nullptr) {
        return copy_out(cppan::dump(*error));
      }
      const cppan::Json* value = result.find("ok");
      if (value == nullptr || !value->is_number()) {
        return copy_out(cppan::dump(cppan::error_json("TypeError", "rank needs numeric results")));
      }
      const double number = value->as_number();
      if (number >= minimum) scored.emplace_back(number, index);
    }
    std::sort(scored.begin(), scored.end(), [](const auto& left, const auto& right) {
      if (left.first != right.first) return left.first > right.first;
      return left.second < right.second;
    });
    cppan::Json::Array order;
    order.reserve(scored.size());
    for (const auto& [value, index] : scored) {
      (void)value;
      order.push_back(cppan::Json::integer(static_cast<std::int64_t>(index)));
    }
    cppan::Json::Object out;
    out.emplace_back("ok", cppan::Json::array(std::move(order)));
    return copy_out(cppan::dump(cppan::Json::object(std::move(out))));
  } catch (const cppan::PortError& error) {
    return copy_out(cppan::dump(cppan::error_json(error)));
  } catch (const std::exception& error) {
    return copy_out(cppan::dump(cppan::error_json("RuntimeError", error.what())));
  }
}

CPPAN_EXPORT void cppan_batch_word_count_text_close(void* handle) {
  delete static_cast<Handle_word_count_text*>(handle);
}

}  // extern "C"

