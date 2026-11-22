#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "xsv/common.hpp"

namespace xsv::hash {

// 64-bit string hash used by the flat table: splitmix-style finalizer over
// 8-byte lanes. Deterministic across runs, which keeps output ordering stable
// for a fixed input.
[[nodiscard]] std::uint64_t hash_bytes(std::string_view key) noexcept;

// Open-addressing multimap with string keys. The table stores indices into an
// entry arena; duplicate keys form an insertion-ordered chain, matching the
// `HashMap<Vec<u8>, Vec<usize>>` the reference join builds. No deletion:
// build-once, probe-many.
template <class V>
class FlatHashMultiMap {
 public:
  FlatHashMultiMap() { rehash(16); }

  void reserve(std::size_t expected_keys) {
    std::size_t wanted = 16;
    while (wanted * 7 / 10 < expected_keys) wanted <<= 1;
    if (wanted > table_.size()) rehash(wanted);
  }

  void insert(std::string_view key, V value) {
    if (entries_.size() * 10 >= table_.size() * 7) {
      rehash(table_.size() * 2);
    }
    const std::uint64_t h = hash_bytes(key);
    std::size_t slot = static_cast<std::size_t>(h) & (table_.size() - 1);
    while (table_[slot] != 0) {
      const std::uint32_t head = table_[slot] - 1;
      if (entries_[head].hash == h && entries_[head].key == key) {
        const std::uint32_t next = static_cast<std::uint32_t>(entries_.size());
        entries_.push_back(Entry{h, key, std::move(value), 0});
        entries_[entries_[head].tail].next_same = next;
        entries_[head].tail = next;
        return;
      }
      slot = (slot + 1) & (table_.size() - 1);
    }
    const std::uint32_t index = static_cast<std::uint32_t>(entries_.size());
    entries_.push_back(Entry{h, key, std::move(value), 0});
    entries_[index].tail = index;
    table_[slot] = index + 1;
    ++key_count_;
  }

  // Calls fn(value) for every value stored under key, in insertion order.
  template <class F>
  void for_each_match(std::string_view key, F&& fn) const {
    if (table_.empty()) return;
    const std::uint64_t h = hash_bytes(key);
    std::size_t slot = static_cast<std::size_t>(h) & (table_.size() - 1);
    while (table_[slot] != 0) {
      const std::uint32_t head = table_[slot] - 1;
      if (entries_[head].hash == h && entries_[head].key == key) {
        std::uint32_t at = head;
        while (true) {
          fn(entries_[at].value);
          if (entries_[at].next_same == 0) break;
          at = entries_[at].next_same;
        }
        return;
      }
      slot = (slot + 1) & (table_.size() - 1);
    }
  }

  [[nodiscard]] std::size_t size() const { return entries_.size(); }
  [[nodiscard]] std::size_t key_count() const { return key_count_; }
  [[nodiscard]] std::size_t bucket_count() const { return table_.size(); }

 private:
  struct Entry {
    std::uint64_t hash = 0;
    std::string_view key;
    V value{};
    std::uint32_t next_same = 0;
    std::uint32_t tail = 0;
  };

  void rehash(std::size_t buckets) {
    std::size_t wanted = 16;
    while (wanted < buckets) wanted <<= 1;
    std::vector<std::uint32_t> table(wanted, 0);
    for (std::size_t i = 0; i < entries_.size(); ++i) {
      std::size_t slot = static_cast<std::size_t>(entries_[i].hash) &
                         (wanted - 1);
      while (table[slot] != 0) slot = (slot + 1) & (wanted - 1);
      table[slot] = static_cast<std::uint32_t>(i) + 1;
    }
    table_.swap(table);
  }

  std::vector<std::uint32_t> table_;
  std::vector<Entry> entries_;
  std::size_t key_count_ = 0;
};

// Baseline multimap: one vector per key. This is the C++-A control for the
// custom hash table, and it preserves insertion order just like the reference.
template <class V>
class StdHashMultiMap {
 public:
  void reserve(std::size_t expected_keys) { map_.reserve(expected_keys); }

  void insert(std::string_view key, V value) {
    map_[std::string(key)].push_back(std::move(value));
  }

  template <class F>
  void for_each_match(std::string_view key, F&& fn) const {
    const auto it = map_.find(std::string(key));
    if (it == map_.end()) return;
    for (const auto& value : it->second) fn(value);
  }

  [[nodiscard]] std::size_t size() const {
    std::size_t n = 0;
    for (const auto& item : map_) n += item.second.size();
    return n;
  }
  [[nodiscard]] std::size_t key_count() const { return map_.size(); }

 private:
  std::unordered_map<std::string, std::vector<V>> map_;
};

#if XSV_USE_CUSTOM_HASH
template <class V>
using MultiMap = FlatHashMultiMap<V>;
#else
template <class V>
using MultiMap = StdHashMultiMap<V>;
#endif

}  // namespace xsv::hash
