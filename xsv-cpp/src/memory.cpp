#include "xsv/memory.hpp"

#include <algorithm>
#include <memory>
#include <new>

namespace xsv::memory {

void* CountingResource::do_allocate(std::size_t bytes, std::size_t alignment) {
  void* memory = std::pmr::new_delete_resource()->allocate(bytes, alignment);
  live_ += bytes;
  peak_ = std::max(peak_, live_);
  ++allocations_;
  return memory;
}

void CountingResource::do_deallocate(void* p, std::size_t bytes,
                                     std::size_t alignment) {
  std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
  live_ -= bytes;
}

bool CountingResource::do_is_equal(
    const std::pmr::memory_resource& other) const noexcept {
  return this == &other;
}

Arena::Arena(std::size_t initial_bytes) : initial_(initial_bytes) {
#if XSV_USE_PMR
  monotonic_ = std::make_unique<std::pmr::monotonic_buffer_resource>(
      initial_.data(), initial_.size(), &counting_);
  active_ = monotonic_.get();
#else
  active_ = &counting_;
#endif
}

Arena::~Arena() = default;

std::pmr::memory_resource* Arena::resource() noexcept { return active_; }

std::size_t Arena::live_bytes() const noexcept { return counting_.live_bytes(); }

std::size_t Arena::peak_bytes() const noexcept { return counting_.peak_bytes(); }

void Arena::reset() {
#if XSV_USE_PMR
  monotonic_->release();
#endif
}

}  // namespace xsv::memory
