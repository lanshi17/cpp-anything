#pragma once

#include <cstddef>
#include <memory>
#include <memory_resource>
#include <vector>

#include "xsv/common.hpp"

namespace xsv::memory {

// Counts live bytes and peak bytes for one resource. Not thread-safe: an Arena
// belongs to one command/thread, matching how xsv's per-command allocations
// are scoped.
class CountingResource final : public std::pmr::memory_resource {
 public:
  [[nodiscard]] std::size_t live_bytes() const { return live_; }
  [[nodiscard]] std::size_t peak_bytes() const { return peak_; }
  [[nodiscard]] std::size_t allocations() const { return allocations_; }

 private:
  void* do_allocate(std::size_t bytes, std::size_t alignment) override;
  void do_deallocate(void* p, std::size_t bytes,
                     std::size_t alignment) override;
  [[nodiscard]] bool do_is_equal(
      const std::pmr::memory_resource& other) const noexcept override;

  std::size_t live_ = 0;
  std::size_t peak_ = 0;
  std::size_t allocations_ = 0;
};

// Per-command working memory. With XSV_USE_PMR the arena is a monotonic buffer
// resource that is released in one shot; without it, every allocation goes to
// the counting resource directly. The public surface is identical so the
// ablation changes allocation policy, not code paths in the commands.
class Arena {
 public:
  explicit Arena(std::size_t initial_bytes = 64 * 1024);
  ~Arena();
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;
  Arena(Arena&&) = delete;
  Arena& operator=(Arena&&) = delete;

  [[nodiscard]] std::pmr::memory_resource* resource() noexcept;
  [[nodiscard]] std::size_t live_bytes() const noexcept;
  [[nodiscard]] std::size_t peak_bytes() const noexcept;

  void reset();

 private:
  CountingResource counting_;
  std::vector<char> initial_;
  std::unique_ptr<std::pmr::monotonic_buffer_resource> monotonic_;
  std::pmr::memory_resource* active_ = nullptr;
};

}  // namespace xsv::memory
