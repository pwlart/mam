#include "AllocationWatch.hpp"
#include <cstdlib>
#include <new>

namespace {
thread_local mam::testing::AllocationCounts* activeCounts = nullptr;
void* allocate(std::size_t size) {
  if (activeCounts) ++activeCounts->allocations;
  if (void* p = std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}
void* allocateAligned(std::size_t size, std::size_t alignment) {
  if (activeCounts) ++activeCounts->allocations;
  void* result = nullptr;
  if (posix_memalign(&result, alignment, size ? size : 1) == 0) return result;
  throw std::bad_alloc();
}
void release(void* p) noexcept {
  if (p && activeCounts) ++activeCounts->deallocations;
  std::free(p);
}
}

namespace mam::testing {
AllocationWatch::AllocationWatch(AllocationCounts& counts) noexcept : previous_(activeCounts) {
  activeCounts = &counts;
}
AllocationWatch::~AllocationWatch() { activeCounts = previous_; }
}

void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void operator delete(void* p) noexcept { release(p); }
void operator delete[](void* p) noexcept { release(p); }
void operator delete(void* p, std::size_t) noexcept { release(p); }
void operator delete[](void* p, std::size_t) noexcept { release(p); }
void* operator new(std::size_t size, std::align_val_t alignment) {
  return allocateAligned(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
  return allocateAligned(size, static_cast<std::size_t>(alignment));
}
void operator delete(void* p, std::align_val_t) noexcept { release(p); }
void operator delete[](void* p, std::align_val_t) noexcept { release(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { release(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { release(p); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  try { return allocate(size); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  try { return allocate(size); } catch (...) { return nullptr; }
}
void operator delete(void* p, const std::nothrow_t&) noexcept { release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { release(p); }
void* operator new(std::size_t size, std::align_val_t a, const std::nothrow_t&) noexcept {
  try { return allocateAligned(size, static_cast<std::size_t>(a)); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t size, std::align_val_t a, const std::nothrow_t&) noexcept {
  try { return allocateAligned(size, static_cast<std::size_t>(a)); } catch (...) { return nullptr; }
}
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { release(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { release(p); }
