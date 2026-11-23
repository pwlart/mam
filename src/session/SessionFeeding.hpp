#pragma once

#include "EventSchedule.hpp"
#include <limits>
#include <span>

namespace mam::session {

inline SampleTime lookaheadEnd(SampleTime now, SampleTime frames) noexcept {
  return frames > std::numeric_limits<SampleTime>::max() - now
      ? std::numeric_limits<SampleTime>::max() : now + frames;
}

// Run before starting the consumer. Reject an unsupported initial horizon;
// waiting for space here cannot make progress. A partial queue is never played
// after this failure. Larger total sessions remain bounded by the cursor.
template <size_t Capacity, typename KeepRunning>
EventSchedule::PumpResult primeSessionQueue(EventSchedule& schedule,
    SpscCommandQueue<Capacity>& queue, SampleTime untilExclusive, KeepRunning&& keepRunning) {
  const auto result = schedule.pump(untilExclusive,
      [&](const Command& command) { return queue.push(command); }, queue.capacity(),
      std::forward<KeepRunning>(keepRunning));
  const auto next = schedule.nextFrame();
  if (!result.stopped && next && *next < untilExclusive)
    throw std::length_error("Realtime session startup horizon exceeds event queue capacity; reduce event density");
  return result;
}

// A callback-local cursor over the already time-ordered, drained commands.
// Late groups execute at the first block boundary, retaining their original
// chronological order. Within each original timestamp, retain the session's
// xfader -> parameters -> triggers policy. Rewriting late timestamps first
// would incorrectly move a later gain change before an earlier trigger.
// The span and its borrowed strings remain owned by the caller. No allocation
// or payload mutation occurs here; repeated boundary calls cannot replay events.
class SessionEventDispatcher {
public:
  SessionEventDispatcher(std::span<const Command> events, SampleTime blockStart) noexcept
      : events_(events), blockStart_(blockStart) {}

  // Call once at block start, then at every in-block event boundary. Return the
  // number of late commands consumed by this call, not the number of handlers.
  template <typename HandleXfader, typename HandleParameter, typename HandleTrigger>
  uint64_t dispatchAt(SampleTime boundary, HandleXfader&& xfader,
                      HandleParameter&& parameter, HandleTrigger&& trigger) {
    uint64_t late = 0;
    while (next_ < events_.size() && events_[next_].sampleTime <= boundary) {
      const SampleTime originalFrame = events_[next_].sampleTime;
      size_t end = next_ + 1;
      while (end < events_.size() && events_[end].sampleTime == originalFrame) ++end;
      const auto group = events_.subspan(next_, end - next_);
      for (const auto& event : group) if (isParameter(event)) xfader(event);
      for (const auto& event : group) if (isParameter(event)) parameter(event);
      for (const auto& event : group)
        if (event.type == CommandType::Trigger) trigger(event);
      if (originalFrame < blockStart_) late += group.size();
      next_ = end;
    }
    return late;
  }

private:
  static bool isParameter(const Command& event) noexcept {
    return event.type == CommandType::SetParam || event.type == CommandType::SetParamRamp;
  }

  std::span<const Command> events_;
  SampleTime blockStart_;
  size_t next_ = 0;
};

} // namespace mam::session
