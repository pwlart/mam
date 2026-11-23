#include "session/EventSchedule.hpp"
#include <doctest/doctest.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using mam::session::EventSchedule;
using mam::session::ScheduledEvent;

namespace {
ScheduledEvent event(SampleTime frame, const char* node, SampleTime period = 0,
                     CommandType type = CommandType::Trigger, float value = 0.0f) {
  ScheduledEvent result;
  result.frame = frame;
  result.nodeId = node;
  result.periodFrames = period;
  result.type = type;
  result.value = value;
  return result;
}
}

TEST_CASE("session cursor merges different loop lengths and one shots exactly once") {
  // B repeats every 3 frames; A every 5. A-tail deliberately begins after its
  // own first period, so a one-cursor-per-rack concatenation would misorder it.
  EventSchedule schedule({event(0, "A", 5), event(1, "B", 3),
                          event(7, "A-tail", 5), event(6, "one-shot")});
  std::vector<std::pair<SampleTime, std::string>> actual;
  const auto sink = [&](const Command& command) {
    actual.emplace_back(command.sampleTime, command.nodeId);
    return true;
  };
  CHECK(schedule.pump(6, sink, 100).enqueued == 4);
  CHECK(schedule.pump(16, sink, 100).enqueued == 8);
  const std::vector<std::pair<SampleTime, std::string>> expected{
      {0, "A"}, {1, "B"}, {4, "B"}, {5, "A"},
      {6, "one-shot"}, {7, "A-tail"}, {7, "B"},
      {10, "A"}, {10, "B"}, {12, "A-tail"}, {13, "B"}, {15, "A"}};
  CHECK(actual == expected);
  CHECK(schedule.nextFrame() == 16);
}

TEST_CASE("session cursor keeps same-frame parameter ordering and stable ties") {
  auto first = event(4, "rack:kick", 0, CommandType::SetParam, .5f);
  first.paramId = 7;
  first.paramName = "GAIN";
  first.source = 1;
  auto equal = first;
  equal.source = 2;
  auto ramp = first;
  ramp.type = CommandType::SetParamRamp;
  ramp.rampMs = 10;
  EventSchedule schedule({event(4, "rack:kick"), ramp, first, equal});
  std::vector<Command> actual;
  const auto result = schedule.pump(5, [&](const Command& command) {
    actual.push_back(command);
    return true;
  }, 8);
  REQUIRE(actual.size() == 4);
  CHECK(actual[0].type == CommandType::SetParam);
  CHECK(actual[1].type == CommandType::SetParam);
  CHECK(actual[0].source == 1);
  CHECK(actual[1].source == 2);
  CHECK(actual[2].type == CommandType::SetParamRamp);
  CHECK(actual[3].type == CommandType::Trigger);
  CHECK(actual[0].paramId == 7);
  CHECK(std::strcmp(actual[0].paramNameStr, "GAIN") == 0);
  CHECK(actual[2].rampMs == 10);
  CHECK_FALSE(result.pending);
}

TEST_CASE("session cursor resolves overlapping-tail collisions by base insertion order") {
  auto first = event(0, "rack:voice", 4, CommandType::SetParamRamp, .5f);
  first.paramId = 7;
  first.rampMs = 20;
  auto tail = first;
  tail.frame = 4;
  tail.rampMs = 10;
  EventSchedule schedule({first, tail});
  std::vector<std::pair<SampleTime, float>> actual;
  const auto sink = [&](const Command& command) {
    actual.emplace_back(command.sampleTime, command.rampMs);
    return true;
  };

  CHECK(schedule.pump(4, sink, 10).enqueued == 1);
  // At frame 4, first's second occurrence precedes tail's first occurrence.
  // Collision precedence uses base insertion, not occurrence age or ramp time;
  // the later-inserted tail wins at this boundary and every following period.
  CHECK(schedule.pump(5, sink, 1).enqueued == 1);
  CHECK(schedule.nextFrame() == 4);
  CHECK(schedule.pump(13, sink, 10).enqueued == 5);
  const std::vector<std::pair<SampleTime, float>> expected{
      {0, 20}, {4, 20}, {4, 10}, {8, 20}, {8, 10}, {12, 20}, {12, 10}};
  CHECK(actual == expected);
  CHECK(schedule.nextFrame() == 16);
}

TEST_CASE("session cursor returns at real startup capacity and resumes the rejected event") {
  for (const size_t count : {size_t{16382}, size_t{16383}, size_t{16384}}) {
    CAPTURE(count);
    auto queue = std::make_unique<SpscCommandQueue<16384>>();
    std::vector<ScheduledEvent> inputs;
    for (size_t i = 0; i < count; ++i)
      inputs.push_back(event(0, "rack:kick", 0, CommandType::SetParam, static_cast<float>(i)));
    EventSchedule schedule(std::move(inputs));
    const auto sink = [&](const Command& command) { return queue->push(command); };
    const auto initial = schedule.pump(1, sink, count + 1);
    CHECK(initial.enqueued == std::min(count, queue->capacity()));
    CHECK(initial.full == (count > queue->capacity()));
    CHECK(initial.pending == (count > queue->capacity()));
    std::vector<Command> actual;
    actual.reserve(count);
    queue->drainUpTo(1, actual);
    const auto refill = schedule.pump(1, sink, count + 1);
    CHECK_FALSE(refill.full);
    CHECK_FALSE(refill.pending);
    queue->drainUpTo(1, actual);
    REQUIRE(actual.size() == count);
    for (size_t i = 0; i < count; ++i) CHECK(actual[i].value == static_cast<float>(i));
  }
}

TEST_CASE("session cursor observes exclusive horizons work budgets and repeated backpressure") {
  EventSchedule schedule({event(0, "loop", 2), event(100, "late-one-shot")});
  std::vector<SampleTime> actual;
  const auto sink = [&](const Command& command) { actual.push_back(command.sampleTime); return true; };
  CHECK(schedule.pump(0, sink, 10).enqueued == 0);
  CHECK(schedule.pump(10, sink, 0).enqueued == 0);
  CHECK(schedule.pump(10, sink, 2).enqueued == 2);
  CHECK(schedule.nextFrame() == 4);
  for (int retry = 0; retry < 3; ++retry) {
    const auto full = schedule.pump(10, [](const Command&) { return false; }, 10);
    CHECK(full.full);
    CHECK(full.enqueued == 0);
    CHECK(schedule.nextFrame() == 4);
  }
  CHECK(schedule.pump(10, sink, 10).enqueued == 3);
  CHECK(actual == std::vector<SampleTime>{0, 2, 4, 6, 8});
  CHECK(schedule.nextFrame() == 10);
}

TEST_CASE("session cursor cancellation retains all not yet accepted events") {
  EventSchedule schedule({event(0, "loop", 1)});
  bool running = true;
  std::vector<SampleTime> actual;
  const auto result = schedule.pump(10, [&](const Command& command) {
    actual.push_back(command.sampleTime);
    if (actual.size() == 3) running = false;
    return true;
  }, 100, [&] { return running; });
  CHECK(result.stopped);
  CHECK(result.enqueued == 3);
  CHECK(schedule.nextFrame() == 3);
  CHECK(schedule.pump(10, [](const Command&) { return true; }, 100,
                      [&] { return running; }).enqueued == 0);
  running = true;
  const auto fullAndStopped = schedule.pump(10, [&](const Command&) {
    running = false;
    return false;
  }, 100, [&] { return running; });
  CHECK(fullAndStopped.full);
  CHECK(fullAndStopped.stopped);
  CHECK(schedule.nextFrame() == 3);
}

TEST_CASE("session cursor owns queued target strings after input lifetime ends") {
  auto schedule = [] {
    auto input = event(0, "rack:temporary-node");
    input.paramName = "temporary-parameter-name";
    return std::make_unique<EventSchedule>(std::vector<ScheduledEvent>{input});
  }();
  Command accepted;
  schedule->pump(1, [&](const Command& command) { accepted = command; return true; }, 1);
  CHECK(std::strcmp(accepted.nodeId, "rack:temporary-node") == 0);
  CHECK(std::strcmp(accepted.paramNameStr, "temporary-parameter-name") == 0);
  CHECK_FALSE(schedule->nextFrame().has_value());
  // Pointer ownership remains valid even when a one-shot's cursor is exhausted.
  CHECK(std::strcmp(accepted.nodeId, "rack:temporary-node") == 0);
}

TEST_CASE("session cursor prevents repetition overflow and handles empty schedules") {
  const SampleTime maximum = std::numeric_limits<SampleTime>::max();
  EventSchedule schedule({event(maximum - 4, "end", 3)});
  std::vector<SampleTime> actual;
  const auto result = schedule.pump(maximum, [&](const Command& command) {
    actual.push_back(command.sampleTime);
    return true;
  }, 10);
  CHECK(actual == std::vector<SampleTime>{maximum - 4, maximum - 1});
  CHECK_FALSE(result.pending);
  EventSchedule empty({});
  CHECK_FALSE(empty.pump(maximum, [](const Command&) { return true; }, 10).pending);
  CHECK_FALSE(empty.nextFrame().has_value());
  EventSchedule atLimit({event(maximum, "exclusive-limit")});
  CHECK(atLimit.pump(maximum, [](const Command&) { return true; }, 10).enqueued == 0);
  CHECK(atLimit.nextFrame() == maximum);
}

TEST_CASE("session cursor validates bounded input and comparison values") {
  CHECK_THROWS_AS(EventSchedule({event(0, "a"), event(1, "b")}, 1), std::invalid_argument);
  CHECK_THROWS_AS(EventSchedule({event(0, "")}), std::invalid_argument);
  auto invalid = event(0, "node");
  invalid.value = std::numeric_limits<float>::quiet_NaN();
  CHECK_THROWS_AS(EventSchedule({invalid}), std::invalid_argument);
  invalid = event(0, "node");
  invalid.rampMs = std::numeric_limits<float>::infinity();
  CHECK_THROWS_AS(EventSchedule({invalid}), std::invalid_argument);
  invalid.rampMs = -1;
  CHECK_THROWS_AS(EventSchedule({invalid}), std::invalid_argument);
  invalid = event(0, "node");
  invalid.type = static_cast<CommandType>(255);
  CHECK_THROWS_AS(EventSchedule({invalid}), std::invalid_argument);
}
