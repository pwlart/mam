#include "offline/OfflineTopoScheduler.hpp"
#include <doctest/doctest.h>
#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>

namespace {
using namespace std::chrono_literals;

class GeneratedNode final : public Node {
public:
  explicit GeneratedNode(float value, bool fail = false, SampleTime failAt = 0)
      : value_(value), fail_(fail), failAt_(failAt) {}
  const char* name() const override { return "GeneratedNode"; }
  void prepare(double, uint32_t) override {}
  void reset() override {}
  void process(ProcessContext context, float* output, uint32_t channels) override {
    std::fill_n(output, static_cast<size_t>(context.frames) * channels, value_);
    if (fail_ && context.blockStart >= failAt_) throw std::runtime_error("generated node failure");
    ++completed;
  }
  unsigned completed = 0;
private:
  float value_;
  bool fail_;
  SampleTime failAt_;
};

struct SiblingGate {
  std::promise<void> entered;
  std::shared_future<void> enteredFuture = entered.get_future().share();
  std::promise<void> release;
  std::shared_future<void> releaseFuture = release.get_future().share();
  std::promise<void> throwing;
  std::future<void> throwingFuture = throwing.get_future();
  std::atomic<bool> finished{false};
};

class GatedNode final : public Node {
public:
  GatedNode(SiblingGate& gate, bool fail) : gate_(gate), fail_(fail) {}
  const char* name() const override { return "GatedNode"; }
  void prepare(double, uint32_t) override {}
  void reset() override {}
  void process(ProcessContext context, float* output, uint32_t channels) override {
    if (fail_) {
      if (gate_.enteredFuture.wait_for(2s) != std::future_status::ready)
        throw std::runtime_error("test sibling did not enter before timeout");
      gate_.throwing.set_value();
      throw std::runtime_error("worker failed while sibling was active");
    }
    gate_.entered.set_value();
    if (gate_.releaseFuture.wait_for(2s) != std::future_status::ready)
      throw std::runtime_error("test sibling gate was not released before timeout");
    std::fill_n(output, static_cast<size_t>(context.frames) * channels, 0.5f);
    gate_.finished.store(true, std::memory_order_release);
  }
private:
  SiblingGate& gate_;
  bool fail_;
};
}

TEST_CASE("offline topo failure waits for every accepted sibling before escaping") {
  SiblingGate gate;
  Graph graph;
  // The failing node is the first future observed by the render thread.
  graph.addNode("throws", std::make_unique<GatedNode>(gate, true));
  graph.addNode("blocked", std::make_unique<GatedNode>(gate, false));
  auto pending = std::make_unique<GeneratedNode>(0.125f);
  auto* finishedPending = pending.get();
  graph.addNode("pending", std::move(pending));
  OfflineTopoScheduler scheduler(2);
  scheduler.setParallelism(2);
  scheduler.setParallelHeuristics(2, 1);
  scheduler.setBlockSize(128);
  scheduler.enableCpuStats(true);
  std::vector<float> output;
  auto result = std::async(std::launch::async, [&] {
    try {
      scheduler.render(graph, {}, {}, 48000, 2, 128, output);
      return std::string("unexpected success");
    } catch (const std::runtime_error& error) {
      if (!gate.finished.load(std::memory_order_acquire)) return std::string("escaped before sibling finished");
      return std::string(error.what());
    }
  });
  const auto throwing = gate.throwingFuture.wait_for(2s);
  const auto beforeRelease = result.wait_for(50ms);
  // Always open the gate before assertions/stack unwinding, including failures.
  gate.release.set_value();
  REQUIRE(throwing == std::future_status::ready);
  CHECK(beforeRelease == std::future_status::timeout);
  REQUIRE(result.wait_for(2s) == std::future_status::ready);
  CHECK(result.get() == "worker failed while sibling was active");
  CHECK(gate.finished.load(std::memory_order_acquire));
  CHECK(finishedPending->completed == 1);
  CHECK(output.empty());
  CHECK(scheduler.getCpuSummary().blocks == 0);
}

TEST_CASE("offline topo failed render clears output and retries with fresh topology and larger buffers") {
  OfflineTopoScheduler scheduler(2);
  scheduler.setParallelism(2);
  scheduler.setParallelHeuristics(2, 1);
  scheduler.setBlockSize(64);
  Graph broken;
  broken.addNode("bad", std::make_unique<GeneratedNode>(0.25f, true));
  auto sibling = std::make_unique<GeneratedNode>(0.5f);
  auto* finishedSibling = sibling.get();
  broken.addNode("sibling", std::move(sibling));
  std::vector<float> output(8, 123.0f);
  CHECK_THROWS_WITH_AS(scheduler.render(broken, {}, {}, 48000, 2, 64, output),
      "generated node failure", std::runtime_error);
  CHECK(finishedSibling->completed == 1);
  CHECK(output.empty());

  Graph replacement;
  replacement.addNode("first", std::make_unique<GeneratedNode>(0.25f));
  replacement.addNode("second", std::make_unique<GeneratedNode>(0.5f));
  replacement.addNode("third", std::make_unique<GeneratedNode>(-0.125f));
  scheduler.setBlockSize(256);
  REQUIRE_NOTHROW(scheduler.render(replacement, {}, {}, 48000, 2, 256, output));
  REQUIRE(output.size() == 512);
  CHECK(std::all_of(output.begin(), output.end(), [](float sample) { return sample == 0.625f; }));
}

TEST_CASE("offline topo serial failure clears partial audio and retry updates buffer channels") {
  OfflineTopoScheduler scheduler(1);
  scheduler.setBlockSize(64);
  Graph broken;
  // A real first block is mixed before the second block fails.
  broken.addNode("bad", std::make_unique<GeneratedNode>(1.0f, true, 64));
  std::vector<float> output(4, 17.0f);
  CHECK_THROWS_AS(scheduler.render(broken, {}, {}, 44100, 1, 128, output), std::runtime_error);
  CHECK(output.empty());
  Graph replacement;
  replacement.addNode("good", std::make_unique<GeneratedNode>(0.125f));
  REQUIRE_NOTHROW(scheduler.render(replacement, {}, {}, 44100, 2, 256, output));
  REQUIRE(output.size() == 512);
  CHECK(std::all_of(output.begin(), output.end(), [](float sample) { return sample == 0.125f; }));
}
