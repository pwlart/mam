#include "core/Graph.hpp"
#include "core/NodeFactory.hpp"
#include "offline/OfflineTimelineRenderer.hpp"
#include "offline/OfflineTopoScheduler.hpp"
#include "session/SessionRuntime.hpp"
#include <doctest/doctest.h>
#include <array>
#include <barrier>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <numeric>
#include <string>
#include <vector>
#include <unistd.h>

namespace {
constexpr uint32_t noiseFrames = 6149;
constexpr uint32_t noiseChannels = 2;
constexpr uint32_t fixtureSeed = 314159u;

// These are generated instruments only. Nonzero noiseMix on BOTH chips and
// two claps expose shared-RNG ordering; four independent nodes exercise four
// workers. Constant parameters exclude the separate legacy ramp limitations.
std::vector<NodeSpec> noiseNodes() {
  std::vector<NodeSpec> nodes(4);
  nodes[0].id = "chip-low"; nodes[0].type = "mam_chip";
  nodes[0].paramsJson = R"({"noiseMix":0.65,"note":48,"gain":0.18,"pan":-0.35,"attackMs":1,"decayMs":35,"sustain":0.6})";
  nodes[1].id = "chip-high"; nodes[1].type = "mam_chip";
  nodes[1].paramsJson = R"({"noiseMix":0.9,"note":60,"gain":0.14,"pan":0.4,"attackMs":0.5,"decayMs":25,"sustain":0.5})";
  nodes[2].id = "clap-a"; nodes[2].type = "clap";
  nodes[2].paramsJson = R"({"ampDecayMs":45,"gain":0.25,"loop":false})";
  nodes[3].id = "clap-b"; nodes[3].type = "clap";
  nodes[3].paramsJson = R"({"ampDecayMs":70,"gain":0.2,"loop":false})";
  return nodes;
}

std::vector<GraphSpec::CommandSpec> noiseCommands(const std::string& prefix = {}) {
  std::vector<GraphSpec::CommandSpec> result;
  for (const SampleTime frame : {0u, 19u, 257u, 1023u, 4097u, 6001u}) {
    for (const auto& spec : noiseNodes()) {
      GraphSpec::CommandSpec event;
      event.sampleTime = frame; event.nodeId = prefix + spec.id; event.type = "Trigger";
      result.push_back(std::move(event));
    }
  }
  return result;
}

// Observe each real instrument before graph summation: reordering floating
// point additions is intentionally not an RNG-invariance requirement.
class CapturedNoise final : public Node {
public:
  explicit CapturedNoise(std::unique_ptr<Node> node) : node_(std::move(node)) {}
  const char* name() const override { return node_->name(); }
  void setRandomSeed(uint32_t seed) override { node_->setRandomSeed(seed); }
  void prepare(double rate, uint32_t frames) override { node_->prepare(rate, frames); }
  void reset() override { node_->reset(); audio.clear(); }
  void handleEvent(const Command& event) override { node_->handleEvent(event); }
  void process(ProcessContext context, float* output, uint32_t channels) override {
    node_->process(context, output, channels);
    audio.insert(audio.end(), output, output + static_cast<size_t>(context.frames) * channels);
  }
  std::vector<float> audio;
private:
  std::unique_ptr<Node> node_;
};

struct NoisyGraph {
  Graph graph;
  std::map<std::string, CapturedNoise*> nodes;
};

NoisyGraph makeNoisyGraph(uint32_t seed, const std::string& scope = {},
                         const std::vector<size_t>& order = {0, 1, 2, 3},
                         const std::string& prefix = {}, bool silentExtra = false) {
  NoisyGraph result;
  result.graph.setRandomSeed(seed, scope);
  const auto specs = noiseNodes();
  if (silentExtra) {
    NodeSpec extra;
    extra.id = "silent-noise-consumer"; extra.type = "mam_chip";
    extra.paramsJson = R"({"noiseMix":1,"gain":0})";
    result.graph.addNode(prefix + extra.id, createNodeFromSpec(extra), extra.id);
  }
  for (const size_t i : order) {
    auto captured = std::make_unique<CapturedNoise>(createNodeFromSpec(specs.at(i)));
    result.nodes.emplace(specs[i].id, captured.get());
    result.graph.addNode(prefix + specs[i].id, std::move(captured), specs[i].id);
  }
  return result;
}

// Disable process-global progress configuration before starting any test
// threads; restore it only after they have joined. Rendering state is local.
struct QuietOffline {
  bool progress = gOfflineProgressEnabled, summary = gOfflineSummaryEnabled;
  QuietOffline() { gOfflineProgressEnabled = false; gOfflineSummaryEnabled = false; }
  ~QuietOffline() { gOfflineProgressEnabled = progress; gOfflineSummaryEnabled = summary; }
};

std::vector<float> baseline(Graph& graph, uint32_t rate, const std::string& prefix = {}) {
  return renderGraphWithCommands(graph, noiseCommands(prefix), rate, noiseChannels, noiseFrames);
}

std::vector<float> topology(Graph& graph, uint32_t rate, uint32_t block, uint32_t workers) {
  OfflineTopoScheduler scheduler(noiseChannels);
  scheduler.setBlockSize(block);
  scheduler.setParallelism(workers, 2);
  scheduler.setParallelHeuristics(2, 1);
  std::vector<float> output;
  scheduler.render(graph, {}, noiseCommands(), rate, noiseChannels, noiseFrames, output);
  return output;
}

// The real baseline renderer uses 1024-frame blocks; topo clamps at 64.
// Exercise the graph itself at 17 frames as well, with the same exact integer
// event boundaries. This helper delivers only the fixture's Trigger events.
std::vector<float> graphChunks(Graph& graph, uint32_t rate, uint32_t block, bool prepare = true) {
  if (prepare) graph.prepare(rate, block);
  graph.reset();
  const auto events = noiseCommands();
  std::vector<float> output(static_cast<size_t>(noiseFrames) * noiseChannels);
  size_t next = 0;
  SampleTime start = 0;
  while (start < noiseFrames) {
    while (next < events.size() && events[next].sampleTime == start) {
      Command event;
      event.sampleTime = start; event.nodeId = events[next].nodeId.c_str();
      graph.forEachNode([&](const std::string& id, Node& node) {
        if (id == event.nodeId) node.handleEvent(event);
      });
      ++next;
    }
    SampleTime end = std::min<SampleTime>(start + block, noiseFrames);
    if (next < events.size()) end = std::min(end, events[next].sampleTime);
    graph.process({static_cast<double>(rate), static_cast<uint32_t>(end - start), start},
        output.data() + static_cast<size_t>(start) * noiseChannels, noiseChannels);
    start = end;
  }
  return output;
}

void checkSignal(const std::vector<float>& audio) {
  CHECK(audio.size() == static_cast<size_t>(noiseFrames) * noiseChannels);
  CHECK(std::all_of(audio.begin(), audio.end(), [](float value) { return std::isfinite(value); }));
  CHECK(std::any_of(audio.begin(), audio.end(), [](float value) { return value != 0; }));
}

struct NoiseScratch {
  std::filesystem::path path;
  NoiseScratch() {
    auto pattern = (std::filesystem::temp_directory_path() / "mam-random-XXXXXX").string();
    const char* made = mkdtemp(pattern.data());
    if (!made) throw std::runtime_error("Cannot create generated noise test directory");
    path = made;
  }
  ~NoiseScratch() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};

std::filesystem::path writeRack(const NoiseScratch& scratch, const char* name, uint32_t seed) {
  nlohmann::json document{{"kind", "rack"}, {"randomSeed", seed},
      {"nodes", nlohmann::json::array()}, {"commands", nlohmann::json::array()}};
  for (const auto& node : noiseNodes())
    document["nodes"].push_back({{"id", node.id}, {"type", node.type},
        {"params", nlohmann::json::parse(node.paramsJson)}});
  for (const auto& command : noiseCommands())
    document["commands"].push_back({{"sampleTime", command.sampleTime},
        {"nodeId", command.nodeId}, {"type", command.type}});
  const auto path = scratch.path / name;
  std::ofstream output(path);
  output << document.dump(2);
  output.close();
  if (!output) throw std::runtime_error("Cannot write generated noise rack");
  return path;
}

SessionSpec sessionFor(const std::filesystem::path& rackPath) {
  SessionSpec session;
  session.sampleRate = 48000; session.channels = noiseChannels;
  SessionSpec::RackRef rack;
  rack.id = "room-left"; rack.path = rackPath.string(); session.racks.push_back(rack);
  rack.id = "room-right"; session.racks.push_back(rack);
  return session;
}

std::map<std::string, std::vector<float>> renderRacks(SessionRuntime& runtime) {
  std::map<std::string, std::vector<float>> outputs;
  for (auto& rack : runtime.racks)
    outputs.emplace(rack.id, renderGraphWithCommands(rack.graph, rack.cmds,
        runtime.sampleRate, runtime.channels, noiseFrames));
  return outputs;
}
}

TEST_CASE("seeded noisy graph matches baseline serial and four-worker renderers across block sizes") {
  QuietOffline quiet;
  for (const uint32_t rate : {44100u, 48000u}) {
    CAPTURE(rate);
    auto reference = makeNoisyGraph(fixtureSeed);
    const auto expected = baseline(reference.graph, rate);
    checkSignal(expected);
    auto shortBlocks = makeNoisyGraph(fixtureSeed);
    CHECK(graphChunks(shortBlocks.graph, rate, 17) == expected);
    for (const uint32_t block : {64u, 256u}) {
      for (const uint32_t workers : {1u, 4u}) {
        CAPTURE(block); CAPTURE(workers);
        auto graph = makeNoisyGraph(fixtureSeed);
        CHECK(topology(graph.graph, rate, block, workers) == expected);
      }
    }
  }
}

TEST_CASE("seeded noisy graph resets replays and changes PCM when its root seed changes") {
  QuietOffline quiet;
  for (const uint32_t rate : {44100u, 48000u}) {
    CAPTURE(rate);
    auto graph = makeNoisyGraph(fixtureSeed);
    const auto first = graphChunks(graph.graph, rate, 17);
    CHECK(graphChunks(graph.graph, rate, 256, false) == first); // reset only
    auto recreated = makeNoisyGraph(fixtureSeed);
    CHECK(baseline(recreated.graph, rate) == first);
    graph.graph.setRandomSeed(fixtureSeed + 1);
    CHECK(baseline(graph.graph, rate) != first);
    graph.graph.setRandomSeed(fixtureSeed);
    CHECK(baseline(graph.graph, rate) == first);
  }
}

TEST_CASE("seeded individual noise streams ignore node order and extra silent consumers") {
  QuietOffline quiet;
  auto reference = makeNoisyGraph(fixtureSeed, "stable-room");
  baseline(reference.graph, 48000);
  auto changed = makeNoisyGraph(fixtureSeed, "stable-room", {3, 1, 0, 2}, {}, true);
  topology(changed.graph, 48000, 64, 4);
  for (const auto& [id, node] : reference.nodes) {
    CAPTURE(id);
    CHECK(changed.nodes.at(id)->audio == node->audio);
  }
  // Otherwise identical chips with different stable IDs receive distinct
  // streams under a nonzero root. Zero intentionally selects shared defaults.
  NodeSpec same = noiseNodes()[0]; same.paramsJson = R"({"noiseMix":1,"gain":0.25,"attackMs":1})";
  Graph graph;
  graph.setRandomSeed(fixtureSeed, "stable-room");
  graph.addNode("one", createNodeFromSpec(same));
  graph.addNode("two", createNodeFromSpec(same));
  graph.prepare(48000, 128); graph.reset();
  Command trigger;
  std::array<float, 256> one{}, two{};
  graph.nodeAt(0)->handleEvent(trigger); graph.nodeAt(1)->handleEvent(trigger);
  graph.nodeAt(0)->process({48000, 128, 0}, one.data(), noiseChannels);
  graph.nodeAt(1)->process({48000, 128, 0}, two.data(), noiseChannels);
  CHECK(one != two);
}

TEST_CASE("seeded graph supports seeding existing and future nodes and invalidates prepared state") {
  QuietOffline quiet;
  auto early = makeNoisyGraph(fixtureSeed, "room");
  auto late = makeNoisyGraph(0);
  late.graph.setRandomSeed(fixtureSeed, "room");
  CHECK(baseline(early.graph, 48000) == baseline(late.graph, 48000));
  late.graph.prepareRealtime(48000, 16, noiseChannels);
  late.graph.setRandomSeed(fixtureSeed + 1, "room");
  std::array<float, 32> buffer; buffer.fill(99);
  CHECK(late.graph.processPrepared({48000, 16, 0}, buffer, noiseChannels) == Graph::PreparedProcessStatus::NotPrepared);
  CHECK(std::all_of(buffer.begin(), buffer.end(), [](float value) { return value == 0; }));
  auto changed = makeNoisyGraph(fixtureSeed + 1, "room");
  CHECK(baseline(late.graph, 48000) == baseline(changed.graph, 48000));
}

TEST_CASE("independent seeded renders have no shared random state under concurrency") {
  QuietOffline quiet;
  auto referenceA = makeNoisyGraph(fixtureSeed, "A");
  auto referenceB = makeNoisyGraph(fixtureSeed + 7, "B");
  const auto expectedA = baseline(referenceA.graph, 44100);
  const auto expectedB = baseline(referenceB.graph, 48000);
  std::barrier begin(3);
  const auto run = [&](uint32_t seed, const char* scope, uint32_t rate, uint32_t block) {
    begin.arrive_and_wait();
    auto graph = makeNoisyGraph(seed, scope);
    return topology(graph.graph, rate, block, 4);
  };
  auto a = std::async(std::launch::async, run, fixtureSeed, "A", 44100u, 64u);
  auto b = std::async(std::launch::async, run, fixtureSeed + 7, "B", 48000u, 256u);
  begin.arrive_and_wait();
  // get() joins completion and propagates worker failures; no sleep-based proof.
  CHECK(a.get() == expectedA);
  CHECK(b.get() == expectedB);
}

TEST_CASE("session rack namespaces preserve local seeds across routing prefixes and rack reorder") {
  QuietOffline quiet;
  NoiseScratch scratch;
  const auto path = writeRack(scratch, "noise.json", fixtureSeed);
  auto spec = sessionFor(path);
  SessionRuntime runtime; runtime.loadFromSpec(spec);
  const auto first = renderRacks(runtime);
  REQUIRE(first.size() == 2);
  checkSignal(first.at("room-left"));
  CHECK(first.at("room-left") != first.at("room-right"));
  for (const auto& [scope, audio] : first) {
    CAPTURE(scope);
    auto local = makeNoisyGraph(fixtureSeed, scope);
    CHECK(baseline(local.graph, 48000) == audio);
    // Realtime session construction prefixes routing IDs but retains ns.id
    // as randomStreamId. Exercise that exact Graph API without opening audio.
    auto prefixed = makeNoisyGraph(fixtureSeed, scope, {0, 1, 2, 3}, scope + ":");
    CHECK(baseline(prefixed.graph, 48000, scope + ":") == audio);
  }
  std::reverse(spec.racks.begin(), spec.racks.end());
  runtime.loadFromSpec(spec);
  CHECK(renderRacks(runtime) == first);
}

TEST_CASE("session seed override wins over rack JSON while zero keeps its fallback semantics") {
  QuietOffline quiet;
  NoiseScratch scratch;
  const auto firstPath = writeRack(scratch, "first.json", fixtureSeed);
  const auto secondPath = writeRack(scratch, "second.json", fixtureSeed + 1);
  SessionRuntime first, second;
  first.loadFromSpec(sessionFor(firstPath), 9876);
  second.loadFromSpec(sessionFor(secondPath), 9876);
  const auto overridden = renderRacks(first);
  CHECK(renderRacks(second) == overridden);
  first.loadFromSpec(sessionFor(firstPath), 0);
  second.loadFromSpec(sessionFor(secondPath), 0);
  CHECK(renderRacks(first) != renderRacks(second));
  auto expected = makeNoisyGraph(9876, "room-left");
  CHECK(overridden.at("room-left") == baseline(expected.graph, 48000));
  const auto defaultPath = writeRack(scratch, "default.json", 0);
  first.loadFromSpec(sessionFor(defaultPath), 0);
  const auto defaults = renderRacks(first);
  // Root zero preserves documented instrument defaults rather than deriving
  // independent IDs/scopes. A nonzero user seed selects namespaced streams.
  CHECK(defaults.at("room-left") == defaults.at("room-right"));
  auto defaultGraph = makeNoisyGraph(0);
  CHECK(defaults.at("room-left") == baseline(defaultGraph.graph, 48000));
}
