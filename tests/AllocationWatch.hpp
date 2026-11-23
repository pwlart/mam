#pragma once
#include <cstddef>

namespace mam::testing {
struct AllocationCounts {
  std::size_t allocations = 0;
  std::size_t deallocations = 0;
};

// Test-only, current-thread C++ allocation instrumentation. Construct fixtures
// and assertion messages before/after this scope. It does not monitor C malloc,
// arbitrary system calls, other threads or realtime deadlines.
class AllocationWatch {
public:
  explicit AllocationWatch(AllocationCounts& counts) noexcept;
  ~AllocationWatch();
  AllocationWatch(const AllocationWatch&) = delete;
  AllocationWatch& operator=(const AllocationWatch&) = delete;
private:
  AllocationCounts* previous_;
};
}
