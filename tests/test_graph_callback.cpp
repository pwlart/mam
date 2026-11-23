#include "realtime/RealtimeGraphRenderer.hpp"
#include <doctest/doctest.h>
#include <array>

namespace {
class GraphEventProbe final : public Node {
public:
  const char* name() const override { return "GraphEventProbe"; }
  void prepare(double, uint32_t) override {}
  void reset() override { triggers = 0; value = 0.25f; }
  void process(ProcessContext context, float* output, uint32_t channels) override {
    std::fill_n(output, static_cast<size_t>(context.frames) * channels, value);
  }
  void handleEvent(const Command& command) override {
    if (command.type == CommandType::Trigger) {
      if (triggers < values.size()) values[triggers] = value;
      ++triggers;
    } else value = command.value;
  }
  uint64_t triggers = 0;
  float value = 0.25f;
  std::array<float, 8> values{};
};

Command probeEvent(SampleTime frame, CommandType type = CommandType::Trigger, float value = 0.0f) {
  Command command;
  command.sampleTime = frame;
  command.nodeId = "probe";
  command.type = type;
  command.value = value;
  return command;
}
}

TEST_CASE("prepared graph callback delivers chronological late groups once") {
  Graph graph;
  auto probe = std::make_unique<GraphEventProbe>();
  auto* observed = probe.get();
  graph.addNode("probe", std::move(probe));
  SpscCommandQueue<2048> queue;
  RealtimeGraphRenderer renderer;
  renderer.setCommandQueue(&queue);
  renderer.prepareGraph(graph, 48000.0, 8, 2);
  std::array<float, 16> output{};
  REQUIRE(renderer.renderBlock(output, 8) == RealtimeGraphRenderer::RenderStatus::Ok);
  REQUIRE(queue.push(probeEvent(1, CommandType::SetParam, 0.3f)));
  REQUIRE(queue.push(probeEvent(2)));
  REQUIRE(queue.push(probeEvent(3, CommandType::SetParam, 0.7f)));
  REQUIRE(queue.push(probeEvent(4)));
  REQUIRE(renderer.renderBlock(output, 8) == RealtimeGraphRenderer::RenderStatus::Ok);
  CHECK(observed->triggers == 2);
  CHECK(observed->values[0] == 0.3f);
  CHECK(observed->values[1] == 0.7f);
  CHECK(renderer.lateEventCount() == 4);
  CHECK(output[0] == 0.7f);
  CHECK(renderer.sampleCounter() == 16);
  REQUIRE(renderer.renderBlock(output, 8) == RealtimeGraphRenderer::RenderStatus::Ok);
  CHECK(observed->triggers == 2);
  CHECK(renderer.lateEventCount() == 4);
}

TEST_CASE("prepared graph callback rejects oversized and short buffers without consuming events") {
  Graph graph;
  auto probe = std::make_unique<GraphEventProbe>();
  auto* observed = probe.get();
  graph.addNode("probe", std::move(probe));
  SpscCommandQueue<2048> queue;
  RealtimeGraphRenderer renderer;
  renderer.setCommandQueue(&queue);
  renderer.prepareGraph(graph, 48000.0, 8, 2);
  REQUIRE(queue.push(probeEvent(0)));
  std::array<float, 18> output;
  output.fill(9.0f);
  CHECK(renderer.renderBlock(output, 9) == RealtimeGraphRenderer::RenderStatus::CapacityExceeded);
  CHECK(std::all_of(output.begin(), output.end(), [](float sample) { return sample == 0.0f; }));
  CHECK(renderer.renderBlock(std::span<float>(output).first(3), 8) == RealtimeGraphRenderer::RenderStatus::InvalidBuffer);
  CHECK(renderer.renderBlock({}, 0) == RealtimeGraphRenderer::RenderStatus::Ok);
  CHECK(observed->triggers == 0);
  CHECK(renderer.sampleCounter() == 0);
  CHECK(renderer.faultCount() == 2);
  REQUIRE(renderer.renderBlock(output, 8) == RealtimeGraphRenderer::RenderStatus::Ok);
  CHECK(observed->triggers == 1);
  CHECK(renderer.sampleCounter() == 8);
}

TEST_CASE("prepared graph full diagnostic queue never drops musical triggers") {
  Graph graph;
  auto probe = std::make_unique<GraphEventProbe>();
  auto* observed = probe.get();
  graph.addNode("probe", std::move(probe));
  SpscCommandQueue<2048> queue;
  RealtimeGraphRenderer renderer;
  renderer.setCommandQueue(&queue);
  renderer.setDiagnostics(true, 120.0, 16);
  renderer.setCmdQueueDebug(true);
  renderer.setDiagLoop(32);
  renderer.prepareGraph(graph, 48000.0, 32, 2);
  for (size_t i = 0; i < queue.capacity(); ++i) REQUIRE(queue.push(probeEvent(0)));
  std::array<float, 64> output{};
  REQUIRE(renderer.renderBlock(output, 32) == RealtimeGraphRenderer::RenderStatus::Ok);
  CHECK(observed->triggers == 2047);
  CHECK(queue.approxSizeProducer() == 0);
  CHECK(renderer.droppedTelemetryCount() == 1026);
  CHECK(renderer.faultCount() == 0);
  CHECK(renderer.lateEventCount() == 0);
}

TEST_CASE("prepared graph callback reports invalid target and requires preparation before reuse") {
  Graph graph;
  graph.addNode("probe", std::make_unique<GraphEventProbe>());
  SpscCommandQueue<2048> queue;
  RealtimeGraphRenderer renderer;
  renderer.setCommandQueue(&queue);
  renderer.prepareGraph(graph, 48000.0, 8, 2);
  auto unknown = probeEvent(0);
  unknown.nodeId = "missing";
  REQUIRE(queue.push(unknown));
  std::array<float, 16> output;
  output.fill(1.0f);
  CHECK(renderer.renderBlock(output, 8) == RealtimeGraphRenderer::RenderStatus::GraphFailure);
  CHECK(std::all_of(output.begin(), output.end(), [](float sample) { return sample == 0.0f; }));
  CHECK(renderer.faultCount() == 1);
  CHECK(renderer.renderBlock(output, 8) == RealtimeGraphRenderer::RenderStatus::NotPrepared);
  renderer.prepareGraph(graph, 48000.0, 8, 2);
  CHECK(renderer.renderBlock(output, 8) == RealtimeGraphRenderer::RenderStatus::Ok);
  renderer.setTransportEmitEnabled(true);
  CHECK_THROWS_AS(renderer.prepareGraph(graph, 48000.0, 8, 2), std::invalid_argument);
}
