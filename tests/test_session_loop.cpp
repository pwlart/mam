#include "session/SessionRuntime.hpp"
#include "offline/OfflineProgress.hpp"
#include <doctest/doctest.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
// Deliberately different L/R values reveal both overlapping loop writes and
// channel swaps. Values and expected sequences below are exact binary fractions.
class LoopPatternNode final : public Node {
public:
  explicit LoopPatternNode(bool waitForTrigger = false) : gated_(waitForTrigger) {}
  const char* name() const override { return "LoopPatternNode"; }
  void prepare(double, uint32_t) override { ++preparations; }
  void reset() override {
    ++resets;
    phase_ = 0;
    active_ = !gated_;
  }
  void handleEvent(const Command& command) override {
    if (command.type != CommandType::Trigger) return;
    ++triggers;
    phase_ = 0;
    active_ = true;
  }
  void process(ProcessContext context, float* output, uint32_t channels) override {
    for (uint32_t frame = 0; frame < context.frames; ++frame) {
      const float value = active_ ? static_cast<float>(++phase_) * 0.125f : 0.0f;
      for (uint32_t channel = 0; channel < channels; ++channel)
        output[static_cast<size_t>(frame) * channels + channel] =
            channel == 0 ? value : -2.0f * value;
    }
    renderedFrames += context.frames;
  }
  unsigned preparations = 0;
  unsigned resets = 0;
  unsigned triggers = 0;
  uint64_t renderedFrames = 0;
private:
  bool gated_;
  bool active_ = false;
  uint64_t phase_ = 0;
};

struct LoopFixture {
  SessionRuntime runtime;
  LoopPatternNode* node = nullptr;
};

LoopFixture makeLoopFixture(uint32_t channels, bool waitForTrigger = false) {
  LoopFixture fixture;
  fixture.runtime.channels = channels;
  SessionRuntime::Rack rack;
  rack.id = "pattern";
  auto node = std::make_unique<LoopPatternNode>(waitForTrigger);
  fixture.node = node.get();
  rack.graph.addNode("tone", std::move(node));
  fixture.runtime.racks.push_back(std::move(rack));
  return fixture;
}

void triggerAt(SessionRuntime& runtime, uint64_t frame) {
  GraphSpec::CommandSpec command;
  command.sampleTime = frame;
  command.nodeId = "tone";
  command.type = "Trigger";
  runtime.racks.front().cmds.push_back(command);
}

struct QuietSessionRender {
  const bool progress = gOfflineProgressEnabled;
  const bool summary = gOfflineSummaryEnabled;
  QuietSessionRender() { gOfflineProgressEnabled = false; gOfflineSummaryEnabled = false; }
  ~QuietSessionRender() { gOfflineProgressEnabled = progress; gOfflineSummaryEnabled = summary; }
};
}

TEST_CASE("offline session loops preserve divisible mono repetitions") {
  QuietSessionRender quiet;
  auto fixture = makeLoopFixture(1);
  CHECK(fixture.runtime.renderOfflineWithLoop(6, 2) ==
        std::vector<float>{0.125f, 0.25f, 0.375f, 0.125f, 0.25f, 0.375f});
  CHECK(fixture.node->resets == 2);
  CHECK(fixture.node->renderedFrames == 6);
}

TEST_CASE("offline session loops place stereo frames without overlap or channel swaps") {
  QuietSessionRender quiet;
  auto fixture = makeLoopFixture(2);
  SUBCASE("odd length loop") {
    CHECK(fixture.runtime.renderOfflineWithLoop(6, 2) == std::vector<float>{
        0.125f, -0.25f, 0.25f, -0.5f, 0.375f, -0.75f,
        0.125f, -0.25f, 0.25f, -0.5f, 0.375f, -0.75f});
  }
  SUBCASE("even length loop") {
    CHECK(fixture.runtime.renderOfflineWithLoop(8, 2) == std::vector<float>{
        0.125f, -0.25f, 0.25f, -0.5f, 0.375f, -0.75f, 0.5f, -1.0f,
        0.125f, -0.25f, 0.25f, -0.5f, 0.375f, -0.75f, 0.5f, -1.0f});
  }
  CHECK(fixture.node->resets == 2);
}

TEST_CASE("offline session loops assign remainder frames to the earliest repetitions") {
  QuietSessionRender quiet;
  SUBCASE("mono 10 frames form loops of 4 then 3 then 3") {
    auto fixture = makeLoopFixture(1);
    CHECK(fixture.runtime.renderOfflineWithLoop(10, 3) == std::vector<float>{
        0.125f, 0.25f, 0.375f, 0.5f, 0.125f, 0.25f, 0.375f,
        0.125f, 0.25f, 0.375f});
    CHECK(fixture.node->renderedFrames == 10);
    CHECK(fixture.node->resets == 3);
  }
  SUBCASE("stereo 7 frames form loops of 4 then 3") {
    auto fixture = makeLoopFixture(2);
    CHECK(fixture.runtime.renderOfflineWithLoop(7, 2) == std::vector<float>{
        0.125f, -0.25f, 0.25f, -0.5f, 0.375f, -0.75f, 0.5f, -1.0f,
        0.125f, -0.25f, 0.25f, -0.5f, 0.375f, -0.75f});
    CHECK(fixture.node->renderedFrames == 7);
    CHECK(fixture.node->resets == 2);
  }
}

TEST_CASE("offline session loops preserve channels across render blocks and loop boundaries") {
  QuietSessionRender quiet;
  auto fixture = makeLoopFixture(2);
  const auto output = fixture.runtime.renderOfflineWithLoop(2050, 2);
  REQUIRE(output.size() == 4100);
  // Each 1025-frame repetition crosses the renderer's 1024-frame block size.
  // Check literal boundary values and equality of the two complete repetitions.
  CHECK(output[0] == 0.125f);
  CHECK(output[1] == -0.25f);
  CHECK(output[2046] == 128.0f);
  CHECK(output[2047] == -256.0f);
  CHECK(output[2048] == 128.125f);
  CHECK(output[2049] == -256.25f);
  CHECK(output[2050] == 0.125f);
  CHECK(output[2051] == -0.25f);
  CHECK(output[4098] == 128.125f);
  CHECK(output[4099] == -256.25f);
  CHECK(std::equal(output.begin(), output.begin() + 2050, output.begin() + 2050));
  CHECK(fixture.node->preparations == 2);
  CHECK(fixture.node->resets == 2);
  CHECK(fixture.node->renderedFrames == 2050);
}

TEST_CASE("offline session loops skip empty repetitions when loops exceed frames") {
  QuietSessionRender quiet;
  auto fixture = makeLoopFixture(2);
  uint32_t loops = 5;
  SUBCASE("five requested loops") {}
  SUBCASE("maximum loop count still renders only three nonempty loops") {
    loops = std::numeric_limits<uint32_t>::max();
  }
  CHECK(fixture.runtime.renderOfflineWithLoop(3, loops) == std::vector<float>{
      0.125f, -0.25f, 0.125f, -0.25f, 0.125f, -0.25f});
  CHECK(fixture.node->preparations == 3);
  CHECK(fixture.node->resets == 3);
  CHECK(fixture.node->renderedFrames == 3);
}

TEST_CASE("offline session loops preserve zero frame and zero loop requests") {
  QuietSessionRender quiet;
  auto fixture = makeLoopFixture(2);
  std::vector<SessionRuntime::RackStats> stats(1);
  SUBCASE("zero frames") {
    CHECK(fixture.runtime.renderOfflineWithLoop(0, 3, &stats).empty());
  }
  SUBCASE("zero loops returns requested silence") {
    CHECK(fixture.runtime.renderOfflineWithLoop(6, 0, &stats) == std::vector<float>(12, 0.0f));
  }
  CHECK(stats.empty());
  CHECK(fixture.node->preparations == 0);
  CHECK(fixture.node->resets == 0);
  CHECK(fixture.node->renderedFrames == 0);
}

TEST_CASE("offline session loops with one repetition match direct rendering") {
  QuietSessionRender quiet;
  auto direct = makeLoopFixture(2);
  auto looped = makeLoopFixture(2);
  auto defaulted = makeLoopFixture(2);
  const auto expected = direct.runtime.renderOffline(9);
  REQUIRE(expected.size() == 18);
  CHECK(looped.runtime.renderOfflineWithLoop(9, 1) == expected);
  CHECK(defaulted.runtime.renderOfflineWithLoop(9) == expected);
  CHECK(looped.node->resets == 1);
  CHECK(defaulted.node->resets == 1);
}

TEST_CASE("offline session loops reset and replay local commands without extending tails") {
  QuietSessionRender quiet;
  auto fixture = makeLoopFixture(1, true);
  triggerAt(fixture.runtime, 1);
  triggerAt(fixture.runtime, 3); // Only the four-frame first repetition reaches this event.
  triggerAt(fixture.runtime, 99); // Does not extend either repetition or output.
  CHECK(fixture.runtime.renderOfflineWithLoop(7, 2) == std::vector<float>{
      0.0f, 0.125f, 0.25f, 0.125f, 0.0f, 0.125f, 0.25f});
  CHECK(fixture.node->preparations == 2);
  CHECK(fixture.node->resets == 2);
  CHECK(fixture.node->triggers == 3);
  CHECK(fixture.node->renderedFrames == 7);
}

TEST_CASE("offline session loops repeat rack offset and gain inside every repetition") {
  QuietSessionRender quiet;
  auto fixture = makeLoopFixture(2);
  fixture.runtime.racks.front().startOffsetFrames = 1;
  fixture.runtime.racks.front().gain = 0.5f;
  CHECK(fixture.runtime.renderOfflineWithLoop(6, 2) == std::vector<float>{
      0.0f, 0.0f, 0.0625f, -0.125f, 0.125f, -0.25f,
      0.0f, 0.0f, 0.0625f, -0.125f, 0.125f, -0.25f});
}

TEST_CASE("offline session loop meters describe the final nonempty repetition") {
  QuietSessionRender quiet;
  auto fixture = makeLoopFixture(1);
  fixture.runtime.setPerRackMeters(true);
  fixture.runtime.racks.front().gain = 0.5f; // Existing meters describe pre-gain rack audio.
  std::vector<SessionRuntime::RackStats> stats(1);
  stats.front().id = "stale";
  const auto output = fixture.runtime.renderOfflineWithLoop(10, 3, &stats);
  REQUIRE(output.size() == 10);
  REQUIRE(stats.size() == 1);
  CHECK(stats.front().id == "pattern");
  CHECK(stats.front().peakDb == doctest::Approx(20.0 * std::log10(0.375)));
  CHECK(stats.front().rmsDb == doctest::Approx(20.0 * std::log10(std::sqrt(14.0 / 3.0) / 8.0)));
  fixture.runtime.setPerRackMeters(false);
  fixture.runtime.renderOfflineWithLoop(3, 5, &stats);
  CHECK(stats.empty());
}

TEST_CASE("offline session loops reject zero channels before changing statistics") {
  SessionRuntime runtime;
  runtime.channels = 0;
  std::vector<SessionRuntime::RackStats> stats(1);
  stats.front().id = "preserved";
  CHECK_THROWS_AS(runtime.renderOfflineWithLoop(8, 2, &stats), std::invalid_argument);
  REQUIRE(stats.size() == 1);
  CHECK(stats.front().id == "preserved");
}

TEST_CASE("offline session loops reject unrepresentable sample counts before changing statistics") {
  // An empty session also keeps this regression safe against the old overflow:
  // wrapped frames*channels then returned an empty vector instead of rejecting.
  SessionRuntime runtime;
  runtime.channels = 2;
  std::vector<SessionRuntime::RackStats> stats(1);
  stats.front().id = "preserved";
  uint64_t frames = 0;
  SUBCASE("uint64 sample multiplication would wrap") {
    frames = std::numeric_limits<uint64_t>::max() / 2 + 1;
  }
  SUBCASE("sample count exceeds vector storage limit without wrapping") {
    frames = static_cast<uint64_t>(std::vector<float>().max_size() / 2) + 1;
  }
  CHECK_THROWS_AS(runtime.renderOfflineWithLoop(frames, 1, &stats), std::length_error);
  REQUIRE(stats.size() == 1);
  CHECK(stats.front().id == "preserved");
}
