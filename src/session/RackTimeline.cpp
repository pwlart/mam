#include "RackTimeline.hpp"
#include "../core/ParamMap.hpp"
#include "../offline/TransportGenerator.hpp"
#include <unordered_map>

namespace mam::session {
namespace {

uint16_t mapParameter(const std::string& type, const std::string& name) {
  if (type == "kick") return resolveParamIdByName(kKickParamMap, name);
  if (type == "clap") return resolveParamIdByName(kClapParamMap, name);
  if (type == "tb303_ext") return resolveParamIdByName(kTb303ParamMap, name);
  if (type == "mam_chip") return resolveParamIdByName(kMamChipParamMap, name);
  return 0;
}

} // namespace

RackTimeline planRackTimeline(const GraphSpec& graph, const SessionSpec::RackRef& rack,
                              uint32_t sampleRate, bool active) {
  RackTimeline result;
  result.rackId = rack.id;
  result.baseCmds = graph.commands;
  uint32_t effectiveBars = 0;
  if (graph.hasTransport) {
    GraphSpec::Transport transport = graph.transport;
    uint32_t baseBars = graph.transport.lengthBars > 0 ? graph.transport.lengthBars : 1u;
    if (rack.bars > 0) baseBars = rack.bars;
    transport.lengthBars = baseBars;
    effectiveBars = baseBars;
    const auto generated = generateCommandsFromTransport(transport, sampleRate);
    result.baseCmds.insert(result.baseCmds.end(), generated.begin(), generated.end());
  }
  if (!active) result.baseCmds.clear();

  std::unordered_map<std::string, std::string> nodeIdToType;
  for (const auto& node : graph.nodes) nodeIdToType.emplace(node.id, node.type);
  for (auto& command : result.baseCmds) {
    command.nodeId = rack.id + ":" + command.nodeId;
    if (command.paramId == 0 && !command.paramName.empty()) {
      const auto found = nodeIdToType.find(command.nodeId.substr(rack.id.size() + 1));
      const std::string type = found != nodeIdToType.end() ? found->second : std::string();
      command.paramId = mapParameter(type, command.paramName);
    }
  }

  if (graph.hasTransport) {
    const double bpm = graph.transport.bpm > 0.0 ? graph.transport.bpm : 120.0;
    const double secondsPerBar = 4.0 * (60.0 / bpm);
    result.framesPerBar = static_cast<uint64_t>(secondsPerBar * sampleRate + 0.5);
    const uint32_t bars = effectiveBars > 0
        ? effectiveBars : (graph.transport.lengthBars > 0 ? graph.transport.lengthBars : 1u);
    result.loopLen = result.framesPerBar * static_cast<uint64_t>(bars);
  }
  return result;
}

} // namespace mam::session
