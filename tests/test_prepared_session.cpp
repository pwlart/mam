#include "realtime/RealtimeSessionRenderer.hpp"
#include <doctest/doctest.h>
#include <array>
#include <limits>

namespace {
using Renderer = RealtimeSessionRenderer;
using Status = Renderer::RenderStatus;

class SessionSignal final : public Node {
public:
  explicit SessionSignal(float level, bool varying = false) : level_(level), initial_(level), varying_(varying) {}
  const char* name() const override { return "generated_session_signal"; }
  void prepare(double, uint32_t) override {}
  void reset() override { level_ = initial_; triggers = 0; calls = 0; }
  void process(ProcessContext ctx, float* output, uint32_t channels) override {
    if (throwOnProcess) throw 1;
    ++calls;
    for (uint32_t frame = 0; frame < ctx.frames; ++frame)
      for (uint32_t channel = 0; channel < channels; ++channel)
        output[static_cast<size_t>(frame) * channels + channel] =
            varying_ ? sample(ctx.blockStart + frame, channel, level_) : level_;
  }
  void handleEvent(const Command& event) override {
    if (throwOnEvent) throw 2;
    if (event.type == CommandType::Trigger) {
      if (triggers < triggerLevels.size()) triggerLevels[triggers] = level_;
      ++triggers;
    } else level_ = event.value;
  }
  static float sample(SampleTime frame, uint32_t channel, float level) {
    return level * static_cast<float>(static_cast<int>((frame * 7 + channel * 3) % 23) - 11) * .0625f;
  }
  size_t triggers = 0, calls = 0;
  std::array<float, 16> triggerLevels{};
  bool throwOnProcess = false, throwOnEvent = false;
private:
  float level_, initial_;
  bool varying_;
};

SessionSignal& addSignal(Graph& graph, const char* id, float value, bool varying = false) {
  auto node = std::make_unique<SessionSignal>(value, varying);
  auto& result = *node;
  graph.addNode(id, std::move(node));
  return result;
}

template <size_t N>
void enqueue(SpscCommandQueue<N>& queue, SampleTime frame, const char* target,
             CommandType type = CommandType::Trigger, float value = 0) {
  Command command;
  command.sampleTime = frame; command.nodeId = target; command.type = type; command.value = value;
  REQUIRE(queue.push(command));
}

void silent(std::span<const float> values) {
  CHECK(std::all_of(values.begin(), values.end(), [](float value) { return value == 0; }));
}
}

TEST_CASE("Prepared session routes buses and fallback racks with existing mute and solo policy") {
  for (const uint32_t channels : {1u, 2u}) {
    for (const bool solo : {false, true}) {
      CAPTURE(channels); CAPTURE(solo);
      Graph a, b, muted;
      auto& an = addSignal(a, "a:signal", .25f);
      auto& bn = addSignal(b, "b:signal", .5f);
      auto& mn = addSignal(muted, "muted:signal", 10.0f);
      const std::vector<Renderer::Rack> racks{
          {&a, "a", .8f, false, solo}, {&b, "b", .2f, false, false},
          {&muted, "muted", 1.0f, true, false}};
      SessionSpec::BusRef bus; bus.id = "sum";
      // Unknown destinations/sources retain the legacy skip/fallback policy.
      const std::vector<SessionSpec::RouteRef> routes{
          {"a", "sum", .5f}, {"a", "sum", -.25f},
          {"b", "missing", 100.0f}, {"unknown", "sum", 100.0f}, {"muted", "sum", 1.0f}};
      Renderer renderer;
      renderer.prepareSession(racks, {bus}, routes, 48000, 8, channels);
      std::array<float, 20> output; output.fill(-99);
      REQUIRE(renderer.renderBlock(output, 7) == Status::Ok);
      const float expected = solo ? .05f : .15f;
      for (size_t i = 0; i < 7 * channels; ++i) CHECK(output[i] == doctest::Approx(expected));
      for (size_t i = 7 * channels; i < output.size(); ++i) CHECK(output[i] == -99);
      CHECK(an.calls == 1);
      CHECK(bn.calls == (solo ? 0 : 1));
      CHECK(mn.calls == 0);
      CHECK(renderer.sampleCounter() == 7);
    }
  }
}

TEST_CASE("Prepared session dispatch keeps original late groups and sample boundaries") {
  Graph graph;
  auto& signal = addSignal(graph, "rack:signal", .1f);
  SpscCommandQueue<32> queue;
  Renderer renderer;
  renderer.setCommandQueue(&queue);
  renderer.prepareSession({{&graph, "rack"}}, {}, {}, 48000, 16, 1);
  std::array<float, 16> output{};
  enqueue(queue, 0, "rack:signal", CommandType::Trigger);
  enqueue(queue, 0, "rack:signal", CommandType::SetParam, .2f);
  enqueue(queue, 4, "rack:signal", CommandType::SetParam, .3f);
  enqueue(queue, 4, "rack:signal", CommandType::Trigger);
  REQUIRE(renderer.renderBlock(output, 8) == Status::Ok);
  CHECK(signal.triggers == 2);
  CHECK(signal.triggerLevels[0] == .2f); CHECK(signal.triggerLevels[1] == .3f);
  for (size_t i = 0; i < 8; ++i) CHECK(output[i] == (i < 4 ? .2f : .3f));
  // These two timestamp groups arrive after the first block finished.
  enqueue(queue, 2, "rack:signal", CommandType::SetParam, .4f);
  enqueue(queue, 2, "rack:signal", CommandType::Trigger);
  enqueue(queue, 3, "rack:signal", CommandType::SetParam, .8f);
  enqueue(queue, 3, "rack:signal", CommandType::Trigger);
  enqueue(queue, 10, "rack:signal", CommandType::SetParam, .6f);
  REQUIRE(renderer.renderBlock(output, 4) == Status::Ok);
  CHECK(signal.triggers == 4);
  CHECK(signal.triggerLevels[2] == .4f); CHECK(signal.triggerLevels[3] == .8f);
  CHECK(output[0] == .8f); CHECK(output[1] == .8f);
  CHECK(output[2] == .6f); CHECK(output[3] == .6f);
  CHECK(renderer.lateEventCount() == 4);
  REQUIRE(renderer.renderBlock(output, 1) == Status::Ok);
  CHECK(signal.triggers == 4); CHECK(renderer.lateEventCount() == 4);
}

TEST_CASE("Prepared session zero and invalid blocks do not consume events") {
  Graph graph; auto& signal = addSignal(graph, "signal", .2f);
  SpscCommandQueue<8> queue;
  Renderer renderer;
  renderer.setCommandQueue(&queue);
  renderer.prepareSession({{&graph, "rack"}}, {}, {}, 48000, 4, 2);
  enqueue(queue, 0, "signal");
  std::array<float, 12> output; output.fill(-99);
  REQUIRE(renderer.renderBlock(output, 0) == Status::Ok);
  CHECK(output[0] == -99); CHECK(queue.approxSizeProducer() == 1);
  CHECK(renderer.sampleCounter() == 0);
  REQUIRE(renderer.renderBlock(output, 5) == Status::CapacityExceeded);
  silent(output);
  CHECK(queue.approxSizeProducer() == 1); CHECK(renderer.sampleCounter() == 0);
  output.fill(-99);
  REQUIRE(renderer.renderBlock(std::span(output).first(3), 2) == Status::InvalidLayout);
  silent(std::span(output).first(3)); CHECK(output[3] == -99);
  CHECK(queue.approxSizeProducer() == 1); CHECK(renderer.sampleCounter() == 0);
  CHECK(renderer.faultCount() == 2);
  REQUIRE(renderer.renderBlock(output, 4) == Status::Ok);
  CHECK(signal.triggers == 1); CHECK(queue.approxSizeProducer() == 0);
  CHECK(renderer.sampleCounter() == 4);
}

TEST_CASE("Prepared session node faults silence and require preparation before retry") {
  for (const bool eventFault : {false, true}) {
    CAPTURE(eventFault);
    Graph graph; auto& signal = addSignal(graph, "signal", .2f);
    SpscCommandQueue<8> queue;
    Renderer renderer;
    renderer.setCommandQueue(&queue);
    renderer.prepareSession({{&graph, "rack"}}, {}, {}, 48000, 4, 1);
    signal.throwOnEvent = eventFault; signal.throwOnProcess = !eventFault;
    enqueue(queue, 0, "signal");
    std::array<float, 4> output; output.fill(-99);
    REQUIRE(renderer.renderBlock(output, 4) == Status::NodeFailure);
    silent(output); CHECK(renderer.sampleCounter() == 4); CHECK(renderer.faultCount() == 1);
    signal.throwOnEvent = false; signal.throwOnProcess = false;
    enqueue(queue, 4, "signal");
    REQUIRE(renderer.renderBlock(output, 4) == Status::NotPrepared);
    silent(output); CHECK(queue.approxSizeProducer() == 1); CHECK(renderer.sampleCounter() == 4);
    renderer.prepareSession({{&graph, "rack"}}, {}, {}, 48000, 4, 1);
    // Repreparation resets transport/DSP; callers own queued event policy.
    REQUIRE(renderer.renderBlock(output, 4) == Status::Ok);
    CHECK(queue.approxSizeProducer() == 1);
    REQUIRE(renderer.renderBlock(output, 4) == Status::Ok);
    CHECK(signal.triggers == 1);
  }
}

TEST_CASE("Prepared session preserves inert manual crossfaders and ordinary LFO arithmetic") {
  for (const bool lfo : {false, true}) {
    CAPTURE(lfo);
    Graph a, b; addSignal(a, "a:signal", .2f); addSignal(b, "b:signal", .8f);
    const std::vector<Renderer::Rack> racks{{&a, "a"}, {&b, "b"}};
    SessionSpec::XfaderRef xf;
    xf.id = "blend"; xf.racks = {"a", "b"}; xf.law = "linear"; xf.smoothingMs = 0;
    xf.lfo.has = lfo; xf.lfo.freqHz = 3.0f; xf.lfo.phase01 = .875f;
    SpscCommandQueue<8> queue;
    Renderer renderer;
    renderer.setCommandQueue(&queue); renderer.setXfaders({xf}, racks);
    renderer.prepareSession(racks, {}, {}, 64, 16, 1);
    // Legacy normal manual IDs were ignored due to a prefix-length bug. Keep
    // that audible behavior here; malformed bare IDs must also remain safe.
    enqueue(queue, 0, "xfader:blend:x", CommandType::SetParam, 0);
    enqueue(queue, 0, "xfader:", CommandType::SetParam, 1);
    std::array<float, 16> output{};
    double phase = .875;
    for (const uint32_t frames : {1u, 7u, 16u, 3u}) {
      REQUIRE(renderer.renderBlock(output, frames) == Status::Ok);
      phase += 3.0 * static_cast<double>(frames) / 64.0;
      while (phase >= 1.0) phase -= 1.0; // Independent retained legacy reference.
      const double x = lfo ? .5 * (std::sin(2.0 * M_PI * phase) + 1.0) : .5;
      const float expected = .2f * static_cast<float>(1 - x) + .8f * static_cast<float>(x);
      for (uint32_t i = 0; i < frames; ++i) CHECK(output[i] == expected);
    }
  }
}

TEST_CASE("Prepared session rejects nonfinite crossfaders and bounds extreme finite LFO phases") {
  Graph a, b; addSignal(a, "a:signal", .2f); addSignal(b, "b:signal", .8f);
  const std::vector<Renderer::Rack> racks{{&a, "a"}, {&b, "b"}};
  SessionSpec::XfaderRef xf; xf.id = "blend"; xf.racks = {"a", "b"}; xf.lfo.has = true;
  Renderer renderer;
  SUBCASE("nonfinite frequency") {
    xf.lfo.freqHz = INFINITY;
    CHECK_THROWS_AS(renderer.setXfaders({xf}, racks), std::invalid_argument);
  }
  SUBCASE("nonfinite phase") {
    xf.lfo.phase01 = std::numeric_limits<float>::quiet_NaN();
    CHECK_THROWS_AS(renderer.setXfaders({xf}, racks), std::invalid_argument);
  }
  SUBCASE("nonfinite smoothing") {
    xf.smoothingMs = INFINITY;
    CHECK_THROWS_AS(renderer.setXfaders({xf}, racks), std::invalid_argument);
  }
  SUBCASE("overflowing phase increment") {
    xf.lfo.freqHz = std::numeric_limits<float>::max();
    renderer.setXfaders({xf}, racks);
    CHECK_THROWS_AS(renderer.prepareSession(racks, {}, {}, 1e-280, 4, 1), std::invalid_argument);
  }
  SUBCASE("large finite phase and frequency") {
    xf.lfo.phase01 = std::numeric_limits<float>::max();
    xf.lfo.freqHz = std::numeric_limits<float>::max();
    renderer.setXfaders({xf}, racks); renderer.prepareSession(racks, {}, {}, 48000, 4, 1);
    std::array<float, 4> output{};
    REQUIRE(renderer.renderBlock(output, 4) == Status::Ok);
    for (const float value : output) CHECK(std::isfinite(value));
  }
}

TEST_CASE("Prepared session bus ducker matches fresh legacy inserts at every event segment") {
  for (const uint32_t channels : {1u, 2u}) {
    for (const bool dynamicEq : {false, true}) {
      CAPTURE(channels); CAPTURE(dynamicEq);
      Graph main, side;
      addSignal(main, "main:signal", .6f, true);
      addSignal(side, "side:signal", 1.8f, true);
      const std::vector<Renderer::Rack> racks{{&main, "main", .75f}, {&side, "side", 0}};
      SessionSpec::BusRef bus; bus.id = "sum";
      SessionSpec::InsertRef insert;
      insert.id = "duck"; insert.type = "spectral_ducker";
      insert.params = {{"mix", .8f}, {"detectorHpfHz", 60.0f},
          {"applyMode", dynamicEq ? "dynamicEq" : "multiply"}, {"stereoMode", "MidSide"}, {"msSideScale", .6f}};
      // Duplicate entries intentionally sum twice; missing sources stay silent.
      insert.sidechains = {{"one", "side"}, {"two", "missing"}, {"three", "side"}};
      bus.inserts.push_back(insert);
      SpscCommandQueue<16> queue;
      Renderer renderer; renderer.setCommandQueue(&queue);
      renderer.prepareSession(racks, {bus}, {{"main", "sum", .8f}}, 48000, 128, channels);
      SampleTime start = 0;
      std::array<float, 256> actual{}, expected{}, sc{};
      for (const uint32_t frames : {17u, 128u, 33u, 1u}) {
        const uint32_t split = frames / 3;
        if (split) enqueue(queue, start + split, "unknown");
        REQUIRE(renderer.renderBlock(actual, frames) == Status::Ok);
        expected.fill(0); sc.fill(0);
        const std::array<uint32_t, 3> cuts{0, split, frames};
        for (size_t segment = 0; segment < 2; ++segment) {
          const uint32_t count = cuts[segment + 1] - cuts[segment];
          if (count == 0) continue;
          for (uint32_t frame = 0; frame < count; ++frame) {
            for (uint32_t channel = 0; channel < channels; ++channel) {
              const size_t index = static_cast<size_t>(frame) * channels + channel;
              const SampleTime absolute = start + cuts[segment] + frame;
              expected[static_cast<size_t>(cuts[segment]) * channels + index] =
                  SessionSignal::sample(absolute, channel, .6f) * (.75f * .8f);
              const float detector = SessionSignal::sample(absolute, channel, 1.8f);
              sc[index] = detector + detector;
            }
          }
          SpectralDuckerNode legacy;
          legacy.mix = .8f; legacy.scHpfHz = 60;
          legacy.applyMode = dynamicEq ? SpectralDuckerNode::ApplyMode::DynamicEq : SpectralDuckerNode::ApplyMode::Multiply;
          legacy.stereoMode = SpectralDuckerNode::StereoMode::MidSide; legacy.msSideScale = .6f;
          legacy.prepare(48000, std::min(count, 4096u));
          legacy.applySidechain({48000, count, start + cuts[segment]},
              expected.data() + static_cast<size_t>(cuts[segment]) * channels, sc.data(), channels);
        }
        CHECK(std::equal(expected.begin(), expected.begin() + static_cast<ptrdiff_t>(frames * channels), actual.begin()));
        start += frames;
      }
    }
  }
}

TEST_CASE("Prepared session restart restores crossfader phase and resolves rack IDs again") {
  Graph a, b; addSignal(a, "a:signal", .2f); addSignal(b, "b:signal", .8f);
  const std::vector<Renderer::Rack> racks{{&a, "a"}, {&b, "b"}};
  SessionSpec::XfaderRef xf;
  xf.id = "blend"; xf.racks = {"a", "b"}; xf.law = "linear";
  xf.lfo.has = true; xf.lfo.phase01 = .125f; xf.lfo.freqHz = 4;
  Renderer renderer; renderer.setXfaders({xf}, racks);
  renderer.prepareSession(racks, {}, {}, 64, 8, 1);
  std::array<float, 8> first{}, next{}, restarted{};
  REQUIRE(renderer.renderBlock(first, 4) == Status::Ok);
  REQUIRE(renderer.renderBlock(next, 8) == Status::Ok);
  CHECK(first[0] != next[0]);
  renderer.prepareSession({racks[1], racks[0]}, {}, {}, 64, 8, 1);
  REQUIRE(renderer.renderBlock(restarted, 4) == Status::Ok);
  CHECK(std::equal(first.begin(), first.begin() + 4, restarted.begin()));
}
