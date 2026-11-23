#include "app/GraphPlayback.hpp"
#include "session/SessionFeeding.hpp"
#include <doctest/doctest.h>
#include <limits>

namespace {
GraphSpec::CommandSpec trigger(SampleTime frame, const char* id = "kick") {
  GraphSpec::CommandSpec command;
  command.sampleTime = frame;
  command.nodeId = id;
  command.type = "Trigger";
  return command;
}
}

TEST_CASE("graph playback empty and frame-zero sequences finish without repetition") {
  GraphSpec graph;
  CHECK(mam::app::planGraphPlayback(graph, 48000).events.empty());
  graph.commands.push_back(trigger(0));
  auto plan = mam::app::planGraphPlayback(graph, 48000);
  REQUIRE(plan.events.size() == 1);
  CHECK(plan.loopFrames == 0);
  CHECK(plan.events[0].periodFrames == 0);
  mam::session::EventSchedule schedule(std::move(plan.events));
  unsigned calls = 0;
  schedule.pump(48000 * 120, [&](const Command&) { ++calls; return true; }, 100);
  CHECK(calls == 1);
  CHECK_FALSE(schedule.nextFrame());
}

TEST_CASE("graph playback explicit repetition preserves distinct coincident triggers and ends at sixty seconds") {
  GraphSpec graph;
  graph.commands = {trigger(0), trigger(48000)};
  auto plan = mam::app::planGraphPlayback(graph, 48000);
  REQUIRE(plan.events.size() == 2);
  CHECK(plan.loopFrames == 48000);
  CHECK(plan.events[0].endFrameExclusive == 2880000);
  mam::session::EventSchedule schedule(std::move(plan.events));
  std::vector<SampleTime> frames;
  schedule.pump(5760000, [&](const Command& command) {
    frames.push_back(command.sampleTime); return true;
  }, 1000);
  REQUIRE(frames.size() == 119);
  CHECK(frames[0] == 0);
  CHECK(frames[1] == 48000);
  CHECK(frames[2] == 48000);
  CHECK(frames.back() == 2832000);
  CHECK(std::is_sorted(frames.begin(), frames.end()));
  CHECK_FALSE(schedule.nextFrame());
  CHECK(graph.commands.size() == 2);
  CHECK(graph.commands[1].sampleTime == 48000);
}

TEST_CASE("graph playback long explicit sequence is one shot with legacy demo cutoff") {
  GraphSpec graph;
  graph.commands = {trigger(0), trigger(2880000), trigger(3000000)};
  auto plan = mam::app::planGraphPlayback(graph, 48000);
  CHECK(plan.loopFrames == 3000000);
  CHECK(plan.events[0].periodFrames == 0);
  mam::session::EventSchedule schedule(std::move(plan.events));
  unsigned calls = 0;
  schedule.pump(6000000, [&](const Command&) { ++calls; return true; }, 100);
  CHECK(calls == 1);
  CHECK_FALSE(schedule.nextFrame());
}

TEST_CASE("graph playback retains legacy transport periods at the actual sample rate") {
  GraphSpec graph;
  graph.hasTransport = true;
  GraphSpec::TransportPattern pattern;
  pattern.nodeId = "kick";
  pattern.steps = "x...x...x...x...";
  graph.transport.patterns.push_back(pattern);
  for (const uint32_t sampleRate : {48000u, 44100u}) {
    const auto plan = mam::app::planGraphPlayback(graph, sampleRate);
    REQUIRE(plan.events.size() == 4);
    CHECK(plan.loopFrames == sampleRate * 2);
    CHECK(plan.events[0].frame == 0);
    // Characterize the existing per-step truncation at 44.1 kHz. This
    // scheduling migration deliberately does not change transport DSP timing.
    CHECK(plan.events[1].frame == (sampleRate == 48000 ? 24000u : 22048u));
    CHECK(plan.events[3].frame == (sampleRate == 48000 ? 72000u : 66144u));
    CHECK(plan.events[0].periodFrames == sampleRate * 2);
    CHECK(plan.events[0].endFrameExclusive == std::numeric_limits<SampleTime>::max());
  }
}

TEST_CASE("graph startup admits exactly queue capacity and rejects one extra without waiting") {
  for (const size_t count : {2047u, 2048u}) {
    GraphSpec graph;
    graph.commands.assign(count, trigger(0));
    auto plan = mam::app::planGraphPlayback(graph, 48000);
    mam::session::EventSchedule schedule(std::move(plan.events));
    SpscCommandQueue<2048> queue;
    if (count == 2047) {
      CHECK(mam::session::primeSessionQueue(schedule, queue, 144000, [] { return true; }).enqueued == 2047);
      CHECK_FALSE(schedule.nextFrame());
    } else {
      CHECK_THROWS_AS(mam::session::primeSessionQueue(schedule, queue, 144000, [] { return true; }), std::length_error);
      CHECK(schedule.nextFrame() == 0);
    }
  }
}
