#pragma once

#include "../core/Command.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace mam::realtime {

enum class TelemetryKind : uint8_t {
  Command, Block, Loop, RackMeter, BusMeter, Xfader, Fault
};

// Numeric callback records only. Names are resolved/formatted by the control
// thread. Command strings are borrowed: their owners must outlive both queued
// commands and all telemetry referring to them (or a stopped queue reset).
struct TelemetryRecord {
  TelemetryKind kind = TelemetryKind::Block;
  ::Command command{};
  SampleTime frame = 0;
  SampleTime cutoff = 0;
  size_t index = 0;
  size_t count = 0;
  double a = 0.0;
  double b = 0.0;
  double c = 0.0;
};

// Exactly one callback producer and one control-thread consumer. Telemetry may
// be dropped when full; scheduled musical commands must use their own queue.
// Capacity includes one empty sentinel slot. No allocation, formatting or waits.
template <class Record = TelemetryRecord, size_t Capacity = 1024>
class TelemetryQueue {
  static_assert(Capacity > 1, "Telemetry queue needs a slot plus its empty sentinel");
  static_assert(std::is_trivially_copyable_v<Record>, "Telemetry records must be trivially copied");
  static_assert(std::is_trivially_copy_assignable_v<Record>, "Telemetry records must be trivially assigned");

public:
  bool push(const Record& record) noexcept {
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t next = (head + 1) % Capacity;
    if (next == tail_.load(std::memory_order_acquire)) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    records_[head] = record;
    head_.store(next, std::memory_order_release);
    return true;
  }

  bool pop(Record& record) noexcept {
    const size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == head_.load(std::memory_order_acquire)) return false;
    record = records_[tail];
    tail_.store((tail + 1) % Capacity, std::memory_order_release);
    return true;
  }

  uint64_t dropped() const noexcept {
    return dropped_.load(std::memory_order_relaxed);
  }

  static constexpr size_t capacity() noexcept { return Capacity - 1; }

  // Only while producer and consumer are stopped. Existing records are discarded
  // without touching their borrowed pointers; resetting is not synchronization.
  void reset() noexcept {
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
    dropped_.store(0, std::memory_order_relaxed);
  }

private:
  std::array<Record, Capacity> records_{};
  std::atomic<size_t> head_{0};
  std::atomic<size_t> tail_{0};
  std::atomic<uint64_t> dropped_{0};
};

} // namespace mam::realtime
