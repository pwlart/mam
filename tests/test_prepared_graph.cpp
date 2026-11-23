#include <doctest/doctest.h>
#include "core/Graph.hpp"
#include "AllocationWatch.hpp"
#include <array>
#include <limits>

namespace {
class FrameSignal final : public Node {
public:
  explicit FrameSignal(float level = 1.0f) : gain(level) {}
  const char* name() const override { return "generated_frame_signal"; }
  void prepare(double, uint32_t) override { ++preparations; }
  void reset() override {}
  void process(ProcessContext ctx, float* out, uint32_t channels) override {
    if (throwOnProcess) throw 1;
    for (uint32_t frame = 0; frame < ctx.frames; ++frame) {
      const float value = static_cast<float>(static_cast<int>((ctx.blockStart + frame) % 17) - 8);
      for (uint32_t channel = 0; channel < channels; ++channel)
        out[static_cast<size_t>(frame) * channels + channel] =
            gain * (value * 0.03125f + static_cast<float>(channel) * 0.125f);
    }
  }
  void handleEvent(const Command& command) override {
    if (throwOnEvent) throw 2;
    ++events;
    gain = command.value;
  }
  void processInPlace(ProcessContext, float*, uint32_t) override { ++insertCalls; }
  float gain;
  unsigned preparations = 0;
  unsigned events = 0;
  unsigned insertCalls = 0;
  bool throwOnProcess = false;
  bool throwOnEvent = false;
};

Graph makeRoutingGraph(bool spectral) {
  Graph graph;
  // Deliberately add the sink before its sources: insertion and processing
  // orders differ, while final mix order must still match the legacy path.
  graph.addNode("meter", std::make_unique<MeterNode>("compressor"));
  graph.addNode("a", std::make_unique<FrameSignal>(0.37f));
  graph.addNode("b", std::make_unique<FrameSignal>(-0.81f));
  graph.addNode("side", std::make_unique<FrameSignal>(3.1f));
  auto delay = std::make_unique<DelayNode>();
  delay->delayMs = 0.25f;
  delay->feedback = 0.37f;
  delay->mix = 0.63f;
  graph.addNode("delay", std::move(delay));
  if (spectral) {
    auto ducker = std::make_unique<SpectralDuckerNode>();
    ducker->lookaheadMs = 0.2f;
    ducker->applyMode = SpectralDuckerNode::ApplyMode::DynamicEq;
    ducker->stereoMode = SpectralDuckerNode::StereoMode::MidSide;
    graph.addNode("compressor", std::move(ducker));
  } else {
    auto compressor = std::make_unique<CompressorNode>();
    compressor->setParams(-30.0f, 4.0f, 0.1f, 12.0f, 0.3f);
    graph.addNode("compressor", std::move(compressor));
  }
  graph.setConnections({
      {"a", "delay", 73.0f, 25.0f},
      {"b", "delay", 33.0f, 19.0f},
      {"b", "delay", -11.0f, 7.0f},
      {"delay", "compressor", 100.0f, 0.0f},
      {"side", "compressor", 135.0f, 0.0f, 0, 1},
      {"b", "compressor", 51.0f, 3.0f, 0, 2},
      {"compressor", "meter", 100.0f, 0.0f}});
  NodeSpec a; a.id = "a"; a.ports.outputs.push_back({0, "", "audio", 2, ""});
  NodeSpec b; b.id = "b"; b.ports.outputs.push_back({0, "", "audio", 1, ""});
  NodeSpec comp; comp.id = "compressor";
  comp.ports.inputs.push_back({1, "", "audio", 1, "sidechain"});
  graph.setPortDescriptors({a, b, comp});
  graph.setMixer(std::make_unique<MixerNode>(
      std::vector<MixerChannel>{{"a", 0.15f}, {"meter", 0.7f}, {"meter", -0.4f}}, 0.8f, true));
  graph.enableStats(true);
  graph.enableCpuStats(true);
  return graph;
}

using Status = Graph::PreparedProcessStatus;
constexpr ProcessContext context{48000.0, 8, 0};

void checkSilent(std::span<const float> values) {
  CHECK(std::all_of(values.begin(), values.end(), [](float value) { return value == 0.0f; }));
}
}

TEST_CASE("Prepared graph preserves legacy routes, DSP and summation without callback allocations") {
  for (uint32_t channels : {1u, 2u}) {
    for (bool spectral : {false, true}) {
      INFO("channels=" << channels << ", spectral=" << spectral);
      auto legacy = makeRoutingGraph(spectral);
      auto prepared = makeRoutingGraph(spectral);
      legacy.prepare(48000.0, 64);
      prepared.prepareRealtime(48000.0, 64, channels);
      std::array<float, 128> expected{}, actual{};
      SampleTime start = 0;
      for (uint32_t frames : {1u, 7u, 64u, 3u, 31u, 8u, 64u, 1u}) {
        const ProcessContext ctx{48000.0, frames, start};
        const size_t count = static_cast<size_t>(frames) * channels;
        expected.fill(-99.0f);
        actual.fill(-99.0f);
        legacy.process(ctx, expected.data(), channels);
        mam::testing::AllocationCounts counts;
        Status result;
        {
          mam::testing::AllocationWatch watch(counts);
          result = prepared.processPrepared(ctx, actual, channels);
        }
        CHECK(result == Status::Ok);
        CHECK(counts.allocations == 0);
        CHECK(counts.deallocations == 0);
        CHECK(std::equal(expected.begin(), expected.begin() + static_cast<ptrdiff_t>(count), actual.begin()));
        CHECK(std::all_of(actual.begin() + static_cast<ptrdiff_t>(count), actual.end(),
                          [](float value) { return value == -99.0f; }));
        start += frames;
      }
      const auto legacyMeters = legacy.getNodeMeters(channels);
      const auto preparedMeters = prepared.getNodeMeters(channels);
      REQUIRE(legacyMeters.size() == preparedMeters.size());
      for (size_t i = 0; i < legacyMeters.size(); ++i) {
        CHECK(legacyMeters[i].id == preparedMeters[i].id);
        CHECK(legacyMeters[i].peakDb == preparedMeters[i].peakDb);
        CHECK(legacyMeters[i].rmsDb == preparedMeters[i].rmsDb);
      }
      CHECK(prepared.getCpuSummary().blocks == 8);
    }
  }
}

TEST_CASE("Prepared graph preserves zero sidechain and explicit zero sink mixer policy") {
  for (bool mixer : {false, true}) {
    Graph graph;
    graph.addNode("signal", std::make_unique<FrameSignal>());
    auto compressor = std::make_unique<CompressorNode>();
    compressor->setParams(-80.0f, 20.0f, 0.1f, 1.0f, 0.0f);
    graph.addNode("effect", std::move(compressor));
    graph.setConnections({{"signal", "effect", 100.0f, 0.0f}});
    if (mixer) graph.setMixer(std::make_unique<MixerNode>(
        std::vector<MixerChannel>{{"effect", 0.0f}}, 1.0f, false));
    graph.prepareRealtime(48000.0, 8, 2);
    std::array<float, 16> output{};
    REQUIRE(graph.processPrepared(context, output, 2) == Status::Ok);
    // Legacy graph supplies a silent detector when port 1 is absent; it does
    // not self-compress. A zero mixer gain still falls back to unity for sinks.
    CHECK(output[0] == -0.25f);
    CHECK(output[1] == -0.125f);
    CHECK(output[14] == -0.03125f);
    CHECK(output[15] == 0.09375f);
  }
}

TEST_CASE("Prepared graph keeps generic nodes on their legacy generator dispatch") {
  Graph graph;
  graph.addNode("upstream", std::make_unique<FrameSignal>(100.0f));
  graph.addNode("downstream", std::make_unique<FrameSignal>(2.0f));
  graph.setConnections({{"upstream", "downstream", 100.0f, 0.0f}});
  graph.prepareRealtime(48000.0, 8, 2);
  std::array<float, 16> output{};
  REQUIRE(graph.processPrepared(context, output, 2) == Status::Ok);
  CHECK(output[0] == -0.5f);
  CHECK(output[1] == -0.25f);
  CHECK(static_cast<FrameSignal*>(graph.nodeAt(0))->insertCalls == 0);
  CHECK(static_cast<FrameSignal*>(graph.nodeAt(1))->insertCalls == 0);
}

TEST_CASE("Prepared graph validates structural inputs before processing") {
  Graph graph;
  graph.addNode("a", std::make_unique<FrameSignal>());
  SUBCASE("sample rate") { CHECK_THROWS_AS(graph.prepareRealtime(0.0, 8, 2), std::invalid_argument); }
  SUBCASE("nonfinite rate") { CHECK_THROWS_AS(graph.prepareRealtime(INFINITY, 8, 2), std::invalid_argument); }
  SUBCASE("frame capacity") { CHECK_THROWS_AS(graph.prepareRealtime(48000.0, 0, 2), std::invalid_argument); }
  SUBCASE("channel layout") { CHECK_THROWS_AS(graph.prepareRealtime(48000.0, 8, 3), std::invalid_argument); }
  SUBCASE("unknown edge") {
    graph.setConnections({{"a", "missing"}});
    CHECK_THROWS_AS(graph.prepareRealtime(48000.0, 8, 2), std::invalid_argument);
  }
  SUBCASE("cycle") {
    graph.addNode("b", std::make_unique<FrameSignal>());
    graph.setConnections({{"a", "b"}, {"b", "a"}});
    CHECK_THROWS_AS(graph.prepareRealtime(48000.0, 8, 2), std::invalid_argument);
  }
  SUBCASE("self cycle") {
    graph.setConnections({{"a", "a"}});
    CHECK_THROWS_AS(graph.prepareRealtime(48000.0, 8, 2), std::invalid_argument);
  }
  SUBCASE("duplicate ID") {
    graph.addNode("a", std::make_unique<FrameSignal>());
    CHECK_THROWS_AS(graph.prepareRealtime(48000.0, 8, 2), std::invalid_argument);
  }
  SUBCASE("empty ID") {
    graph.addNode("", std::make_unique<FrameSignal>());
    CHECK_THROWS_AS(graph.prepareRealtime(48000.0, 8, 2), std::invalid_argument);
  }
  SUBCASE("null node") {
    graph.addNode("b", nullptr);
    CHECK_THROWS_AS(graph.prepareRealtime(48000.0, 8, 2), std::invalid_argument);
  }
  SUBCASE("nonfinite route") {
    graph.addNode("b", std::make_unique<FrameSignal>());
    graph.setConnections({{"a", "b", std::numeric_limits<float>::quiet_NaN()}});
    CHECK_THROWS_AS(graph.prepareRealtime(48000.0, 8, 2), std::invalid_argument);
  }
  SUBCASE("unknown mixer source") {
    graph.setMixer(std::make_unique<MixerNode>(std::vector<MixerChannel>{{"missing", 1.0f}}));
    CHECK_THROWS_AS(graph.prepareRealtime(48000.0, 8, 2), std::invalid_argument);
  }
  SUBCASE("allocating trace") {
    graph.enableTrace("unused-test-trace.json");
    CHECK_THROWS_AS(graph.prepareRealtime(48000.0, 8, 2), std::invalid_argument);
  }
}

TEST_CASE("Prepared graph guards capacities and silences only caller supplied storage") {
  Graph graph;
  graph.addNode("signal", std::make_unique<FrameSignal>());
  graph.prepareRealtime(48000.0, 8, 2);
  std::array<float, 18> output;
  output.fill(41.0f);
  auto span = std::span<float>(output).subspan(1, 16);
  ProcessContext ctx = context;
  uint32_t channels = 2;
  Status expected = Status::InvalidContext;
  SUBCASE("frames") { ctx.frames = 9; expected = Status::CapacityExceeded; }
  SUBCASE("channels") { channels = 1; expected = Status::InvalidLayout; }
  SUBCASE("rate") { ctx.sampleRate = 44100.0; }
  SUBCASE("nonfinite rate") { ctx.sampleRate = std::numeric_limits<double>::quiet_NaN(); }
  SUBCASE("short span") { span = span.first(15); }
  mam::testing::AllocationCounts counts;
  Status actual;
  {
    mam::testing::AllocationWatch watch(counts);
    actual = graph.processPrepared(ctx, span, channels);
  }
  CHECK(actual == expected);
  CHECK(counts.allocations == 0);
  CHECK(counts.deallocations == 0);
  checkSilent(span);
  CHECK(output.front() == 41.0f);
  CHECK(output.back() == 41.0f);
  if (span.size() == 15) CHECK(output[16] == 41.0f);
}

TEST_CASE("Prepared graph rejects stale plans after stopped engine mutation") {
  Graph graph;
  graph.addNode("signal", std::make_unique<FrameSignal>());
  graph.prepareRealtime(48000.0, 8, 2);
  SUBCASE("node") { graph.addNode("extra", std::make_unique<FrameSignal>()); }
  SUBCASE("connections") { graph.setConnections({}); }
  SUBCASE("ports") { graph.setPortDescriptors({}); }
  SUBCASE("mixer") { graph.setMixer(nullptr); }
  SUBCASE("ordinary preparation") { graph.prepare(48000.0, 8); }
  SUBCASE("topology rebuild") { graph.rebuildTopology(); }
  SUBCASE("trace") { graph.enableTrace("unused-test-trace.json"); }
  std::array<float, 16> output;
  output.fill(1.0f);
  CHECK(graph.processPrepared(context, output, 2) == Status::NotPrepared);
  checkSilent(output);
  Command command; command.nodeId = "signal";
  CHECK_FALSE(graph.handlePreparedEvent(command));
}

TEST_CASE("Prepared event dispatch owns target lookup and does not allocate") {
  Graph graph;
  const std::string longId(128, 'x');
  graph.addNode(longId, std::make_unique<FrameSignal>());
  graph.addNode("other", std::make_unique<FrameSignal>());
  graph.prepareRealtime(48000.0, 8, 2);
  auto* target = static_cast<FrameSignal*>(graph.nodeAt(0));
  auto* other = static_cast<FrameSignal*>(graph.nodeAt(1));
  CHECK(target->preparations == 1);
  Command command; command.nodeId = longId.c_str(); command.value = 2.0f;
  mam::testing::AllocationCounts counts;
  bool delivered, unknown, null;
  {
    mam::testing::AllocationWatch watch(counts);
    delivered = graph.handlePreparedEvent(command);
    command.nodeId = "missing"; unknown = graph.handlePreparedEvent(command);
    command.nodeId = nullptr; null = graph.handlePreparedEvent(command);
  }
  CHECK(delivered);
  CHECK_FALSE(unknown);
  CHECK_FALSE(null);
  CHECK(counts.allocations == 0);
  CHECK(counts.deallocations == 0);
  CHECK(target->events == 1);
  CHECK(target->gain == 2.0f);
  CHECK(other->events == 0);
  // Moving the owner must preserve both long heap strings and short IDs used
  // by precompiled string_view keys.
  Graph moved = std::move(graph);
  command.nodeId = longId.c_str();
  CHECK(moved.handlePreparedEvent(command));
  command.nodeId = "other";
  CHECK(moved.handlePreparedEvent(command));
  CHECK(target->events == 2);
  CHECK(other->events == 1);
}

TEST_CASE("Prepared graph handles zero frames, empty graphs and node failures") {
  Graph graph;
  std::array<float, 16> output;
  output.fill(1.0f);
  SUBCASE("not prepared") {
    CHECK(graph.processPrepared(context, output, 2) == Status::NotPrepared);
    checkSilent(output);
  }
  SUBCASE("empty graph") {
    graph.prepareRealtime(48000.0, 8, 2);
    CHECK(graph.processPrepared(context, output, 2) == Status::Ok);
    checkSilent(output);
  }
  SUBCASE("zero frames touches no output") {
    graph.addNode("signal", std::make_unique<FrameSignal>());
    graph.prepareRealtime(48000.0, 8, 2);
    CHECK(graph.processPrepared({48000.0, 0, 0}, output, 2) == Status::Ok);
    CHECK(std::all_of(output.begin(), output.end(), [](float value) { return value == 1.0f; }));
  }
  SUBCASE("process exception silences and invalidates") {
    auto node = std::make_unique<FrameSignal>(); node->throwOnProcess = true;
    graph.addNode("signal", std::move(node));
    graph.prepareRealtime(48000.0, 8, 2);
    CHECK(graph.processPrepared(context, output, 2) == Status::NodeFailure);
    checkSilent(output);
    CHECK(graph.processPrepared(context, output, 2) == Status::NotPrepared);
  }
  SUBCASE("event exception invalidates") {
    auto node = std::make_unique<FrameSignal>(); node->throwOnEvent = true;
    graph.addNode("signal", std::move(node));
    graph.prepareRealtime(48000.0, 8, 2);
    Command command; command.nodeId = "signal";
    CHECK_FALSE(graph.handlePreparedEvent(command));
    CHECK(graph.processPrepared(context, output, 2) == Status::NotPrepared);
    checkSilent(output);
  }
}
