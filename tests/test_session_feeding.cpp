#include "session/SessionFeeding.hpp"
#include <doctest/doctest.h>
#include <algorithm>
#include <chrono>
#include <future>
#include <memory>
#include <thread>
#include <string_view>

using mam::session::EventSchedule;
using mam::session::ScheduledEvent;
using mam::session::primeSessionQueue;

namespace {
ScheduledEvent makeEvent(SampleTime frame, const char* node, SampleTime period = 0) {
  ScheduledEvent event;
  event.frame = frame;
  event.nodeId = node;
  event.periodFrames = period;
  return event;
}
}

TEST_CASE("session startup admits exactly capacity and rejects excess before playback") {
  for (const size_t count : {size_t{16382}, size_t{16383}, size_t{16384}}) {
    CAPTURE(count);
    auto queue = std::make_unique<SpscCommandQueue<16384>>();
    std::vector<ScheduledEvent> events(count, makeEvent(0, "kick"));
    // A future one-shot must not cause rejection when the initial window fits.
    events.push_back(makeEvent(144000, "future"));
    EventSchedule schedule(std::move(events));
    if (count <= queue->capacity()) {
      const auto result = primeSessionQueue(schedule, *queue, 144000, [] { return true; });
      CHECK(result.enqueued == count);
      CHECK(result.pending);
      CHECK_FALSE(result.stopped);
      CHECK(schedule.nextFrame() == 144000);
    } else {
      CHECK_THROWS_AS(primeSessionQueue(schedule, *queue, 144000, [] { return true; }),
                      std::length_error);
      CHECK(schedule.nextFrame() == 0);
    }
    CHECK(queue->approxSizeProducer() == std::min(count, queue->capacity()));
  }
}

TEST_CASE("session startup cancellation leaves a stopped consumer untouched") {
  EventSchedule schedule({makeEvent(0, "kick", 1)});
  SpscCommandQueue<8> queue;
  const auto result = primeSessionQueue(schedule, queue, 100, [] { return false; });
  CHECK(result.stopped);
  CHECK(result.enqueued == 0);
  CHECK(queue.approxSizeProducer() == 0);
  CHECK(schedule.nextFrame() == 0);
}

TEST_CASE("session startup and refill share an ordered cursor under backpressure") {
  const std::vector<ScheduledEvent> inputs{
      makeEvent(0, "A", 5), makeEvent(1, "B", 3),
      makeEvent(7, "A-tail", 5), makeEvent(90, "one-shot")};
  EventSchedule schedule(inputs);
  SpscCommandQueue<8> queue;
  CHECK(primeSessionQueue(schedule, queue, 6, [] { return true; }).enqueued == 4);
  std::vector<Command> drained;
  std::vector<std::pair<SampleTime, std::string>> actual;
  bool sawFull = false;
  SampleTime cutoff = 0;
  for (unsigned turn = 0; turn < 100; ++turn) {
    const auto result = schedule.pump(110, [&](const Command& c) { return queue.push(c); },
                                      queue.capacity());
    sawFull = sawFull || result.full;
    cutoff = std::min<SampleTime>(110, cutoff + 4);
    drained.clear();
    queue.drainUpTo(cutoff, drained);
    for (const auto& command : drained)
      actual.emplace_back(command.sampleTime, command.nodeId);
    const auto next = schedule.nextFrame();
    if ((!next || *next >= 110) && queue.approxSizeProducer() == 0) break;
  }
  // Independent finite expansion: the production heap/cursor does not compute
  // the expected answer. This includes overlapping tails and a future one-shot.
  std::vector<std::pair<SampleTime, std::string>> expected;
  for (const auto& input : inputs) {
    for (SampleTime frame = input.frame; frame < 110; frame += input.periodFrames) {
      expected.emplace_back(frame, input.nodeId);
      if (input.periodFrames == 0) break;
    }
  }
  std::sort(expected.begin(), expected.end());
  CHECK(sawFull);
  CHECK(actual == expected);
  CHECK(queue.approxSizeProducer() == 0);
  CHECK(schedule.nextFrame() >= 110);
}

TEST_CASE("session producer cancels on a full queue without losing the pending event") {
  EventSchedule schedule({makeEvent(0, "kick", 1)});
  SpscCommandQueue<8> queue;
  primeSessionQueue(schedule, queue, 7, [] { return true; });
  std::promise<EventSchedule::PumpResult> attempted;
  auto attemptedFuture = attempted.get_future();
  std::promise<void> release;
  auto releaseFuture = release.get_future();
  EventSchedule::PumpResult cancelled;
  std::jthread producer([&](std::stop_token stop) {
    const auto keepRunning = [&] { return !stop.stop_requested(); };
    attempted.set_value(schedule.pump(20, [&](const Command& c) { return queue.push(c); },
                                      queue.capacity(), keepRunning));
    releaseFuture.wait(); // Test handshake, not a production queue wait.
    cancelled = schedule.pump(20, [&](const Command& c) { return queue.push(c); },
                               queue.capacity(), keepRunning);
  });
  const auto ready = attemptedFuture.wait_for(std::chrono::seconds(2));
  producer.request_stop();
  release.set_value();
  producer.join();
  REQUIRE(ready == std::future_status::ready);
  const auto full = attemptedFuture.get();
  CHECK(full.full);
  CHECK(full.enqueued == 0);
  CHECK(cancelled.stopped);
  CHECK(cancelled.enqueued == 0);
  CHECK(schedule.nextFrame() == 7);
  std::vector<Command> actual;
  queue.drainUpTo(20, actual);
  schedule.pump(10, [&](const Command& c) { return queue.push(c); }, queue.capacity());
  queue.drainUpTo(20, actual);
  REQUIRE(actual.size() == 10);
  for (size_t i = 0; i < actual.size(); ++i) CHECK(actual[i].sampleTime == i);
}

TEST_CASE("session late dispatch preserves original gain trigger groups exactly once") {
  std::vector<Command> commands;
  for (const auto& [frame, value] : std::vector<std::pair<SampleTime, float>>{
           {10, .4f}, {20, .8f}, {100, .6f}, {164, .9f}}) {
    Command parameter;
    parameter.sampleTime = frame;
    parameter.nodeId = "rack:kick";
    parameter.paramNameStr = "GAIN";
    parameter.type = CommandType::SetParam;
    parameter.paramId = 7;
    parameter.value = value;
    parameter.source = 1;
    commands.push_back(parameter);
    Command trigger = parameter;
    trigger.type = CommandType::Trigger;
    commands.push_back(trigger);
  }
  const auto before = commands;
  mam::session::SessionEventDispatcher dispatch(commands, 100);
  float gain = 0;
  std::vector<float> attacks;
  std::vector<SampleTime> originalTriggerFrames;
  const auto xfader = [](const Command&) {};
  const auto parameter = [&](const Command& event) { gain = event.value; };
  const auto trigger = [&](const Command& event) {
    attacks.push_back(gain);
    originalTriggerFrames.push_back(event.sampleTime);
  };
  CHECK(dispatch.dispatchAt(100, xfader, parameter, trigger) == 4);
  CHECK(attacks == std::vector<float>{.4f, .8f, .6f});
  CHECK(dispatch.dispatchAt(100, xfader, parameter, trigger) == 0);
  CHECK(dispatch.dispatchAt(120, xfader, parameter, trigger) == 0);
  CHECK(attacks == std::vector<float>{.4f, .8f, .6f});
  CHECK(dispatch.dispatchAt(164, xfader, parameter, trigger) == 0);
  CHECK(dispatch.dispatchAt(164, xfader, parameter, trigger) == 0);
  CHECK(attacks == std::vector<float>{.4f, .8f, .6f, .9f});
  CHECK(originalTriggerFrames == std::vector<SampleTime>{10, 20, 100, 164});
  for (size_t i = 0; i < commands.size(); ++i) {
    CHECK(commands[i].sampleTime == before[i].sampleTime);
    CHECK(commands[i].nodeId == before[i].nodeId);
    CHECK(commands[i].paramNameStr == before[i].paramNameStr);
    CHECK(commands[i].type == before[i].type);
    CHECK(commands[i].paramId == before[i].paramId);
    CHECK(commands[i].value == before[i].value);
    CHECK(commands[i].rampMs == before[i].rampMs);
    CHECK(commands[i].source == before[i].source);
  }
}

TEST_CASE("session on time dispatch keeps xfader parameter trigger phases") {
  std::vector<Command> commands(5);
  const char* targets[] = {"rack:a", "rack:a", "xfader:mix:x", "rack:b", "rack:b"};
  const CommandType types[] = {CommandType::Trigger, CommandType::SetParam,
      CommandType::SetParamRamp, CommandType::SetParamRamp, CommandType::Trigger};
  for (size_t i = 0; i < commands.size(); ++i) {
    commands[i].sampleTime = 100;
    commands[i].nodeId = targets[i];
    commands[i].type = types[i];
  }
  mam::session::SessionEventDispatcher dispatch(commands, 100);
  std::vector<std::string> calls;
  const auto xfader = [&](const Command& event) {
    if (std::string_view(event.nodeId).starts_with("xfader:"))
      calls.push_back(std::string("xfader:") + event.nodeId);
  };
  const auto parameter = [&](const Command& event) {
    calls.push_back(std::string("parameter:") + event.nodeId);
  };
  const auto trigger = [&](const Command& event) {
    calls.push_back(std::string("trigger:") + event.nodeId);
  };
  CHECK(dispatch.dispatchAt(99, xfader, parameter, trigger) == 0);
  CHECK(calls.empty());
  CHECK(dispatch.dispatchAt(100, xfader, parameter, trigger) == 0);
  CHECK(calls == std::vector<std::string>{"xfader:xfader:mix:x",
      "parameter:rack:a", "parameter:xfader:mix:x", "parameter:rack:b",
      "trigger:rack:a", "trigger:rack:b"});
  const auto count = calls.size();
  CHECK(dispatch.dispatchAt(120, xfader, parameter, trigger) == 0);
  CHECK(calls.size() == count);
  mam::session::SessionEventDispatcher empty({}, 100);
  CHECK(empty.dispatchAt(100, xfader, parameter, trigger) == 0);
  CHECK(calls.size() == count);
}

TEST_CASE("session lookahead saturates rather than wrapping the absolute clock") {
  const auto maximum = std::numeric_limits<SampleTime>::max();
  CHECK(mam::session::lookaheadEnd(100, 50) == 150);
  CHECK(mam::session::lookaheadEnd(maximum - 10, 9) == maximum - 1);
  CHECK(mam::session::lookaheadEnd(maximum - 10, 10) == maximum);
  CHECK(mam::session::lookaheadEnd(maximum - 10, 11) == maximum);
  CHECK(mam::session::lookaheadEnd(maximum, 0) == maximum);
}
