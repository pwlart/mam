#pragma once

#include "../core/Command.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mam::session {

// A base event is a one-shot when periodFrames == 0. Otherwise it repeats at
// frame + k * periodFrames, including when frame lies beyond the first period.
// Strings belong to the schedule, which must outlive every queued Command.
struct ScheduledEvent {
  SampleTime frame = 0;
  std::string nodeId;
  CommandType type = CommandType::Trigger;
  uint16_t paramId = 0;
  float value = 0.0f;
  float rampMs = 0.0f;
  std::string paramName;
  uint8_t source = 0;
  SampleTime periodFrames = 0;
};

class EventSchedule {
public:
  static constexpr size_t defaultMaximumBaseEvents = 1'000'000;

  struct PumpResult {
    size_t enqueued = 0;
    bool full = false;
    bool pending = false;  // Includes events beyond the requested horizon.
    bool stopped = false;
  };

  explicit EventSchedule(std::vector<ScheduledEvent> events,
                         size_t maximumBaseEvents = defaultMaximumBaseEvents)
      : events_(std::move(events)) {
    if (events_.size() > maximumBaseEvents)
      throw std::invalid_argument("Session schedule exceeds its base-event limit");
    heap_.reserve(events_.size());
    for (size_t i = 0; i < events_.size(); ++i) {
      const auto& event = events_[i];
      if (event.nodeId.empty() || event.nodeId.find('\0') != std::string::npos ||
          event.paramName.find('\0') != std::string::npos)
        throw std::invalid_argument("Session events require valid owned target names");
      if (!std::isfinite(event.value) || !std::isfinite(event.rampMs) || event.rampMs < 0.0f)
        throw std::invalid_argument("Session event values and ramp times must be finite and valid");
      if (event.type != CommandType::Trigger && event.type != CommandType::SetParam &&
          event.type != CommandType::SetParamRamp)
        throw std::invalid_argument("Unknown session event type");
      heap_.push_back({i, event.frame});
    }
    std::make_heap(heap_.begin(), heap_.end(), Later{&events_});
  }

  // Commands carry borrowed pointers into events_. Prevent accidental lifetime
  // changes while the consumer still holds a queued event.
  EventSchedule(const EventSchedule&) = delete;
  EventSchedule& operator=(const EventSchedule&) = delete;
  EventSchedule(EventSchedule&&) = delete;
  EventSchedule& operator=(EventSchedule&&) = delete;

  std::optional<SampleTime> nextFrame() const noexcept {
    if (heap_.empty()) return std::nullopt;
    return heap_.front().frame;
  }

  size_t baseEventCount() const noexcept { return events_.size(); }

  // Producer-side operation: never waits, and uses only constructor-allocated
  // storage. The sink returns false without accepting an event when full.
  // A rejected event stays pending. Use the same instance for startup and refill.
  // maxEvents bounds work even when a consumer continuously frees queue slots.
  // Timeliness/overload policy belongs to the runner; this cursor never drops or
  // rewrites a late timestamp. UINT64_MAX is excluded by an exclusive horizon.
  template <typename TryPush, typename KeepRunning>
  PumpResult pump(SampleTime untilExclusive, TryPush&& tryPush, size_t maxEvents,
                  KeepRunning&& keepRunning) {
    PumpResult result;
    const Later later{&events_};
    while (result.enqueued < maxEvents && !heap_.empty() && heap_.front().frame < untilExclusive) {
      if (!keepRunning()) {
        result.stopped = true;
        break;
      }
      Cursor cursor = heap_.front();
      const auto& event = events_[cursor.event];
      Command command{};
      command.sampleTime = cursor.frame;
      command.nodeId = event.nodeId.c_str();
      command.type = event.type;
      command.paramId = event.paramId;
      command.value = event.value;
      command.rampMs = event.rampMs;
      command.paramNameStr = event.paramName.empty() ? nullptr : event.paramName.c_str();
      command.source = event.source;
      if (!tryPush(command)) {
        result.full = true;
        break;
      }
      ++result.enqueued;
      std::pop_heap(heap_.begin(), heap_.end(), later);
      heap_.pop_back();
      if (event.periodFrames != 0 &&
          event.periodFrames <= std::numeric_limits<SampleTime>::max() - cursor.frame) {
        cursor.frame += event.periodFrames;
        heap_.push_back(cursor);  // Never exceeds the reserved base-event count.
        std::push_heap(heap_.begin(), heap_.end(), later);
      }
    }
    result.pending = !heap_.empty();
    result.stopped = result.stopped || !keepRunning();
    return result;
  }

  template <typename TryPush>
  PumpResult pump(SampleTime untilExclusive, TryPush&& tryPush, size_t maxEvents) {
    return pump(untilExclusive, std::forward<TryPush>(tryPush), maxEvents, [] { return true; });
  }

private:
  struct Cursor {
    size_t event;
    SampleTime frame;
  };

  struct Later {
    const std::vector<ScheduledEvent>* events;

    static unsigned typeOrder(CommandType type) noexcept {
      return type == CommandType::SetParam ? 0u : type == CommandType::SetParamRamp ? 1u : 2u;
    }

    bool operator()(const Cursor& left, const Cursor& right) const {
      if (left.frame != right.frame) return left.frame > right.frame;
      const auto& a = (*events)[left.event];
      const auto& b = (*events)[right.event];
      if (a.nodeId != b.nodeId) return a.nodeId > b.nodeId;
      if (a.type != b.type) return typeOrder(a.type) > typeOrder(b.type);
      if (a.paramId != b.paramId) return a.paramId > b.paramId;
      if (a.value != b.value) return a.value > b.value;
      // Preserve original insertion order for otherwise identical events.
      return left.event > right.event;
    }
  };

  std::vector<ScheduledEvent> events_;
  std::vector<Cursor> heap_;
};

}  // namespace mam::session
