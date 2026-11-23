#include "AllocationWatch.hpp"
#include "core/NodeFactory.hpp"
#include "realtime/RealtimeGraphRenderer.hpp"
#include "realtime/RealtimeSessionRenderer.hpp"
#include <doctest/doctest.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <unistd.h>

namespace {
// Capture actual stdout/stderr output from the measured callback. This checks
// the production diagnostic flags, not arbitrary file/syscall/lock behavior.
class CaptureOutput {
public:
  CaptureOutput() {
    std::fflush(stdout); std::fflush(stderr);
    file_ = std::tmpfile();
    if (!file_) throw std::runtime_error("Cannot create callback output capture");
    savedOut_ = dup(STDOUT_FILENO); savedErr_ = dup(STDERR_FILENO);
    if (savedOut_ < 0 || savedErr_ < 0 || dup2(fileno(file_), STDOUT_FILENO) < 0 ||
        dup2(fileno(file_), STDERR_FILENO) < 0) {
      restore();
      throw std::runtime_error("Cannot redirect callback output capture");
    }
  }
  ~CaptureOutput() { restore(); }
  long bytes() {
    std::fflush(stdout); std::fflush(stderr);
    return std::ftell(file_);
  }
private:
  void restore() noexcept {
    std::fflush(stdout); std::fflush(stderr);
    if (savedOut_ >= 0) { dup2(savedOut_, STDOUT_FILENO); close(savedOut_); savedOut_ = -1; }
    if (savedErr_ >= 0) { dup2(savedErr_, STDERR_FILENO); close(savedErr_); savedErr_ = -1; }
    if (file_) { std::fclose(file_); file_ = nullptr; }
  }
  FILE* file_ = nullptr;
  int savedOut_ = -1, savedErr_ = -1;
};

struct TemporaryMetrics {
  std::string path;
  TemporaryMetrics() {
    path = (std::filesystem::temp_directory_path() / "mam-callback-metrics-XXXXXX").string();
    const int descriptor = mkstemp(path.data());
    if (descriptor < 0) throw std::runtime_error("Cannot create metrics fixture");
    close(descriptor);
  }
  ~TemporaryMetrics() {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }
};

constexpr const char* kickId = "rack-with-a-long-name:kick-with-a-long-name";
std::unique_ptr<Graph> instrumentGraph() {
  auto graph = std::make_unique<Graph>();
  for (const auto* type : {"kick", "clap", "tb303_ext", "mam_chip"}) {
    NodeSpec spec;
    spec.id = (std::string_view(type) == "kick") ? kickId : type;
    spec.type = type;
    spec.paramsJson = "{}";
    graph->addNode(spec.id, createNodeFromSpec(spec));
  }
  auto delay = std::make_unique<DelayNode>();
  delay->delayMs = 2.0f;
  graph->addNode("delay", std::move(delay));
  auto compressor = std::make_unique<SpectralDuckerNode>();
  compressor->applyMode = SpectralDuckerNode::ApplyMode::DynamicEq;
  graph->addNode("ducker", std::move(compressor));
  std::vector<GraphSpec::Connection> edges;
  auto edge = [&](const char* from, const char* to, uint32_t port) {
    GraphSpec::Connection connection;
    connection.from = from; connection.to = to; connection.toPort = port;
    connection.gainPercent = 100;
    edges.push_back(connection);
  };
  edge(kickId, "delay", 0); edge("delay", "ducker", 0); edge("clap", "ducker", 1);
  graph->setConnections(edges);
  return graph;
}

template <size_t N> void triggerAt(SpscCommandQueue<N>& queue, SampleTime frame, const char* target) {
  Command event;
  event.sampleTime = frame; event.nodeId = target;
  REQUIRE(queue.push(event));
}
}

TEST_CASE("callback allocation and output probes detect positive controls") {
  mam::testing::AllocationCounts counts;
  long bytes = 0;
  {
    CaptureOutput capture;
    {
      mam::testing::AllocationWatch watch(counts);
      void* ordinary = ::operator new(64);
      ::operator delete(ordinary);
      void* aligned = ::operator new(128, std::align_val_t{64});
      ::operator delete(aligned, std::align_val_t{64});
      std::fputs("positive diagnostic control\n", stderr);
    }
    bytes = capture.bytes();
  }
  CHECK(counts.allocations == 2);
  CHECK(counts.deallocations == 2);
  CHECK(bytes > 0);
}

TEST_CASE("prepared graph callback first and variable blocks allocate and print nothing") {
  for (const uint32_t channels : {1u, 2u}) {
    CAPTURE(channels);
    auto graph = instrumentGraph();
    SpscCommandQueue<2048> queue;
    RealtimeGraphRenderer renderer;
    renderer.setCommandQueue(&queue);
    renderer.setDiagnostics(true, 120.0, 16);
    renderer.setCmdQueueDebug(true);
    renderer.setDiagLoop(64);
    renderer.prepareGraph(*graph, 48000.0, 128, channels);
    for (const auto* target : {kickId, "clap", "tb303_ext", "mam_chip"}) triggerAt(queue, 0, target);
    triggerAt(queue, 40, kickId);
    triggerAt(queue, 127, kickId);
    std::array<float, 256> output{};
    mam::testing::AllocationCounts counts;
    bool success = true, finite = true;
    long callbackBytes = -1, polledBytes = -1;
    {
      CaptureOutput capture;
      {
        mam::testing::AllocationWatch watch(counts);
        for (const auto frames : {1u, 31u, 128u, 7u, 64u, 2u, 128u}) {
          const auto samples = std::span(output).first(static_cast<size_t>(frames) * channels);
          success &= renderer.renderBlock(samples, frames) == RealtimeGraphRenderer::RenderStatus::Ok;
          for (const auto sample : samples) finite &= std::isfinite(sample);
        }
      }
      callbackBytes = capture.bytes();
      renderer.pollDiagnostics();
      polledBytes = capture.bytes();
    }
    CHECK(success);
    CHECK(finite);
    CHECK(counts.allocations == 0);
    CHECK(counts.deallocations == 0);
    CHECK(callbackBytes == 0);
    CHECK(polledBytes > 0);
  }
}

TEST_CASE("prepared session callback includes bus inserts meters and telemetry without allocation") {
  for (const uint32_t channels : {1u, 2u}) {
    CAPTURE(channels);
    auto graph = instrumentGraph();
    SpscCommandQueue<16384> queue;
    TemporaryMetrics metrics;
    RealtimeSessionRenderer renderer;
    renderer.setCommandQueue(&queue);
    renderer.setDiagnostics(true);
    renderer.setDebug(true);
    renderer.setMeters(true, .06);
    renderer.setMetricsNdjson(metrics.path.c_str(), true, true);
    std::vector<RealtimeSessionRenderer::Rack> racks{{graph.get(), "rack-with-long-id", 1, false, false}};
    SessionSpec::BusRef bus;
    bus.id = "bus-with-long-id"; bus.channels = channels;
    SessionSpec::InsertRef insert;
    insert.id = "ducker-with-long-id"; insert.type = "spectral_ducker";
    insert.params = {{"applyMode", "dynamicEq"}, {"mix", .8f}, {"detectorHpfHz", 60.0f}};
    insert.sidechains.push_back({"sc", racks[0].id});
    bus.inserts.push_back(insert);
    renderer.prepareSession(racks, {bus}, {{racks[0].id, bus.id, .75f}}, 48000, 128, channels);
    for (const auto* target : {kickId, "clap", "tb303_ext", "mam_chip"}) triggerAt(queue, 0, target);
    triggerAt(queue, 40, kickId); triggerAt(queue, 127, kickId);
    std::array<float, 256> output{};
    mam::testing::AllocationCounts counts;
    bool success = true, finite = true;
    long callbackBytes = -1, polledBytes = -1;
    uintmax_t callbackFileBytes = 0, polledFileBytes = 0;
    {
      CaptureOutput capture;
      {
        mam::testing::AllocationWatch watch(counts);
        for (unsigned block = 0; block < 80; ++block) {
          const uint32_t frames = block % 3 == 0 ? 1 : (block % 3 == 1 ? 31 : 128);
          const auto samples = std::span(output).first(static_cast<size_t>(frames) * channels);
          success &= renderer.renderBlock(samples, frames) == RealtimeSessionRenderer::RenderStatus::Ok;
          for (const auto sample : samples) finite &= std::isfinite(sample);
        }
      }
      callbackBytes = capture.bytes();
      // Flush outside the measured callback so any forbidden buffered metrics
      // writes become observable before control-thread polling.
      std::fflush(nullptr);
      callbackFileBytes = std::filesystem::file_size(metrics.path);
      renderer.pollDiagnostics();
      polledBytes = capture.bytes();
      polledFileBytes = std::filesystem::file_size(metrics.path);
    }
    CHECK(success);
    CHECK(finite);
    CHECK(counts.allocations == 0);
    CHECK(counts.deallocations == 0);
    CHECK(callbackBytes == 0);
    CHECK(polledBytes > 0);
    CHECK(callbackFileBytes == 0);
    CHECK(polledFileBytes > 0);
  }
}
