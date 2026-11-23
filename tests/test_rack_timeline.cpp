#include "session/RackTimeline.hpp"
#include <doctest/doctest.h>
#include <array>

namespace {

GraphSpec rackGraph() {
  GraphSpec graph;
  NodeSpec node;
  node.id = "kick";
  node.type = "kick";
  graph.nodes.push_back(node);
  graph.hasTransport = true;
  graph.transport.bpm = 120;
  graph.transport.lengthBars = 2;
  GraphSpec::TransportPattern pattern;
  pattern.nodeId = "kick";
  pattern.steps = "x...x...x...x...";
  graph.transport.patterns.push_back(pattern);
  return graph;
}

SessionSpec::RackRef rackReference() {
  SessionSpec::RackRef rack;
  rack.id = "drums";
  return rack;
}

} // namespace

TEST_CASE("rack timeline keeps explicit commands before prefixed transport events") {
  auto graph = rackGraph();
  GraphSpec::CommandSpec explicitCommand;
  explicitCommand.sampleTime = 96001;
  explicitCommand.nodeId = "kick";
  explicitCommand.type = "SetParamRamp";
  explicitCommand.paramName = "gain";
  explicitCommand.value = .4f;
  explicitCommand.rampMs = 25;
  graph.commands.push_back(explicitCommand);

  const auto plan = mam::session::planRackTimeline(graph, rackReference(), 48000, true);
  CHECK(plan.rackId == "drums");
  CHECK(plan.framesPerBar == 96000);
  CHECK(plan.loopLen == 192000);
  REQUIRE(plan.baseCmds.size() == 9);
  CHECK(plan.baseCmds[0].sampleTime == 96001);
  CHECK(plan.baseCmds[0].nodeId == "drums:kick");
  CHECK(plan.baseCmds[0].type == "SetParamRamp");
  CHECK(plan.baseCmds[0].paramId == 5);
  CHECK(plan.baseCmds[0].value == .4f);
  CHECK(plan.baseCmds[0].rampMs == 25);
  constexpr std::array<uint64_t, 8> expected{0, 24000, 48000, 72000, 96000, 120000, 144000, 168000};
  for (size_t i = 0; i < expected.size(); ++i) {
    CHECK(plan.baseCmds[i + 1].sampleTime == expected[i]);
    CHECK(plan.baseCmds[i + 1].nodeId == "drums:kick");
    CHECK(plan.baseCmds[i + 1].type == "Trigger");
  }
  CHECK(graph.commands[0].nodeId == "kick");
  CHECK(graph.commands[0].paramId == 0);
  CHECK(graph.transport.lengthBars == 2);
}

TEST_CASE("rack bars override affects event count and loop length without changing the graph") {
  const auto graph = rackGraph();
  auto rack = rackReference();
  rack.bars = 1;
  const auto plan = mam::session::planRackTimeline(graph, rack, 48000, true);
  CHECK(plan.framesPerBar == 96000);
  CHECK(plan.loopLen == 96000);
  REQUIRE(plan.baseCmds.size() == 4);
  CHECK(plan.baseCmds.back().sampleTime == 72000);
  CHECK(graph.transport.lengthBars == 2);
}

TEST_CASE("inactive rack timeline clears all events while retaining timing metadata") {
  auto graph = rackGraph();
  GraphSpec::CommandSpec command;
  command.nodeId = "kick";
  command.type = "Trigger";
  graph.commands.push_back(command);
  const auto plan = mam::session::planRackTimeline(graph, rackReference(), 48000, false);
  CHECK(plan.baseCmds.empty());
  CHECK(plan.framesPerBar == 96000);
  CHECK(plan.loopLen == 192000);
  CHECK(graph.commands.size() == 1);
}

TEST_CASE("rack timeline retains legacy fractional-step rounding at 44100 Hz") {
  const auto plan = mam::session::planRackTimeline(rackGraph(), rackReference(), 44100, true);
  CHECK(plan.framesPerBar == 88200);
  CHECK(plan.loopLen == 176400);
  REQUIRE(plan.baseCmds.size() == 8);
  // Existing transport truncates each sixteenth step to 5512 frames. Keep this
  // reference in an extraction; a later timing correction must be deliberate.
  constexpr std::array<uint64_t, 8> expected{0, 22048, 44096, 66144, 88192, 110240, 132288, 154336};
  for (size_t i = 0; i < expected.size(); ++i) CHECK(plan.baseCmds[i].sampleTime == expected[i]);
}

TEST_CASE("rack timeline resolves supported names but preserves explicit and unknown parameter IDs") {
  GraphSpec graph;
  const std::array<std::string, 5> types{"kick", "clap", "tb303_ext", "mam_chip", "future_node"};
  const std::array<std::string, 5> names{"FEND", "PAN", "CUTOFF_HZ", "NOTE_SEMITONES", "GAIN"};
  const std::array<uint16_t, 5> expected{2, 5, 4, 2, 0};
  for (size_t i = 0; i < types.size(); ++i) {
    NodeSpec node;
    node.id = "voice" + std::to_string(i);
    node.type = types[i];
    graph.nodes.push_back(node);
    GraphSpec::CommandSpec command;
    command.nodeId = node.id;
    command.type = "SetParam";
    command.paramName = names[i];
    graph.commands.push_back(command);
  }
  auto explicitId = graph.commands[0];
  explicitId.paramId = 77;
  graph.commands.push_back(explicitId);
  auto unknownNode = graph.commands[0];
  unknownNode.nodeId = "absent";
  graph.commands.push_back(unknownNode);

  auto rack = rackReference();
  rack.bars = 4; // A bar override alone does not invent a transport.
  const auto plan = mam::session::planRackTimeline(graph, rack, 48000, true);
  CHECK(plan.framesPerBar == 0);
  CHECK(plan.loopLen == 0);
  REQUIRE(plan.baseCmds.size() == 7);
  for (size_t i = 0; i < expected.size(); ++i) CHECK(plan.baseCmds[i].paramId == expected[i]);
  CHECK(plan.baseCmds[5].paramId == 77);
  CHECK(plan.baseCmds[6].nodeId == "drums:absent");
  CHECK(plan.baseCmds[6].paramId == 0);
}
