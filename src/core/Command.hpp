#pragma once

#include <cstdint>
#include <atomic>
#include <vector>
#include <cstring>
#include <span>

using SampleTime = uint64_t;
using NodeIdStr = const char*; // stored elsewhere; commands copy into a small buffer if needed

enum class CommandType : uint8_t { Trigger = 0, SetParam, SetParamRamp };

struct Command {
  SampleTime sampleTime = 0;  // absolute
  NodeIdStr nodeId = nullptr; // pointer to stable id string
  CommandType type = CommandType::Trigger;
  uint16_t paramId = 0;       // for SetParam/SetParamRamp
  float value = 0.0f;         // new value
  float rampMs = 0.0f;        // for SetParamRamp
  const char* paramNameStr = nullptr; // optional diagnostics fallback for printing
  uint8_t source = 0;         // 0 = rack (graph/transport), 1 = session
};

template <size_t Capacity>
class SpscCommandQueue {
  static_assert(Capacity > 1, "SPSC queue needs a slot plus its empty sentinel");
public:
  SpscCommandQueue() : head_(0), tail_(0) {}

  bool push(const Command& c) {
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t next = (head + 1) % Capacity;
    if (next == tail_.load(std::memory_order_acquire)) return false; // full
    buffer_[head] = c;
    head_.store(next, std::memory_order_release);
    return true;
  }

  // Producer-side approximate size and capacity (for debugging/telemetry only)
  size_t approxSizeProducer() const {
    const size_t tail = tail_.load(std::memory_order_acquire);
    const size_t head = head_.load(std::memory_order_relaxed);
    return (head >= tail) ? (head - tail) : (Capacity - (tail - head));
  }
  constexpr size_t capacity() const { return Capacity - 1; }

  // Consumer only. Snapshot the producer head once, so a producer refilling
  // freed slots cannot extend this drain. The caller supplies all storage;
  // events beyond its span or at/after cutoff remain queued, in FIFO order.
  // Arrivals after the snapshot are intentionally deferred to the next drain.
  size_t drainUpToBounded(SampleTime cutoff, std::span<Command> out) noexcept {
    size_t tail = tail_.load(std::memory_order_relaxed);
    const size_t end = head_.load(std::memory_order_acquire);
    size_t count = 0;
    while (tail != end && count < out.size()) {
      const Command& command = buffer_[tail];
      if (command.sampleTime >= cutoff) break;
      out[count++] = command;
      tail = (tail + 1) % Capacity;
    }
    tail_.store(tail, std::memory_order_release);
    return count;
  }

  // Drain into out vector all commands with sampleTime < cutoff
  void drainUpTo(SampleTime cutoff, std::vector<Command>& out) {
    size_t tail = tail_.load(std::memory_order_relaxed);
    while (tail != head_.load(std::memory_order_acquire)) {
      const Command& c = buffer_[tail];
      if (c.sampleTime >= cutoff) break;
      out.push_back(c);
      tail = (tail + 1) % Capacity;
      tail_.store(tail, std::memory_order_release);
    }
  }

private:
  Command buffer_[Capacity];
  std::atomic<size_t> head_;
  std::atomic<size_t> tail_;
};
