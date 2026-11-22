#include "xsv/hash.hpp"

#include <cstring>

namespace xsv::hash {
namespace {

constexpr std::uint64_t kSeed = 0x243F6A8885A308D3ULL;

std::uint64_t rotl(std::uint64_t value, int bits) noexcept {
  return (value << bits) | (value >> (64 - bits));
}

std::uint64_t mix(std::uint64_t value) noexcept {
  value ^= value >> 33;
  value *= 0xFF51AFD7ED558CCDULL;
  value ^= value >> 33;
  value *= 0xC4CEB9FE1A85EC53ULL;
  value ^= value >> 33;
  return value;
}

}  // namespace

std::uint64_t hash_bytes(std::string_view key) noexcept {
  const char* data = key.data();
  const std::size_t size = key.size();
  std::uint64_t hash = kSeed ^ size;
  std::size_t at = 0;
  while (at + 8 <= size) {
    std::uint64_t lane = 0;
    std::memcpy(&lane, data + at, 8);
    hash ^= lane;
    hash *= 0x9E3779B97F4A7C15ULL;
    hash = rotl(hash, 31);
    at += 8;
  }
  if (at < size) {
    std::uint64_t lane = 0;
    std::memcpy(&lane, data + at, size - at);
    hash ^= lane;
    hash *= 0x9E3779B97F4A7C15ULL;
    hash = rotl(hash, 27);
  }
  return mix(hash);
}

}  // namespace xsv::hash
