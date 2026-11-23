#pragma once

#include "../core/GraphConfig.hpp"
#include "../core/ParamMap.hpp"
#include "../offline/TransportGenerator.hpp"
#include "../session/EventSchedule.hpp"
#include <unordered_map>

namespace mam::app {

struct GraphPlaybackPlan {
  std::vector<session::ScheduledEvent> events;
  SampleTime loopFrames = 0;
};

// Preparation only: own all event strings and retain the single-graph legacy
// loop rules. Transport repeats indefinitely; explicit-only graphs repeat to
// the exclusive 60-second demo horizon using their last event as the period.
// An empty or frame-zero-only sequence is one-shot, never a zero-step loop.
inline GraphPlaybackPlan planGraphPlayback(const GraphSpec& graph, uint32_t sampleRate) {
  GraphPlaybackPlan result;
  auto commands = graph.commands;
  if (graph.hasTransport) {
    auto transport = graph.transport;
    transport.lengthBars = std::max(transport.lengthBars, 1u);
    const auto generated = generateCommandsFromTransport(transport, sampleRate);
    commands.insert(commands.end(), generated.begin(), generated.end());
    const double bpm = transport.bpm > 0.0 ? transport.bpm : 120.0;
    const auto framesPerBar = static_cast<SampleTime>((4.0 * 60.0 / bpm) * sampleRate + 0.5);
    result.loopFrames = framesPerBar * transport.lengthBars;
  }
  SampleTime baseSpan = 0;
  for (const auto& command : commands) baseSpan = std::max(baseSpan, command.sampleTime);
  if (result.loopFrames == 0) result.loopFrames = baseSpan;
  const SampleTime demoEnd = static_cast<SampleTime>(sampleRate) * 60;
  const bool continuous = graph.hasTransport && result.loopFrames > 0;
  const SampleTime period = continuous || baseSpan < demoEnd ? result.loopFrames : 0;

  std::unordered_map<std::string, std::string> nodeTypes;
  for (const auto& node : graph.nodes) nodeTypes.emplace(node.id, node.type);
  result.events.reserve(commands.size());
  for (const auto& command : commands) {
    session::ScheduledEvent event;
    event.frame = command.sampleTime;
    event.nodeId = command.nodeId;
    if (command.type == "SetParam") event.type = CommandType::SetParam;
    else if (command.type == "SetParamRamp") event.type = CommandType::SetParamRamp;
    // Legacy graph commands default to Trigger for an unrecognized type.
    event.paramId = command.paramId;
    event.paramName = command.paramName;
    event.value = command.value;
    event.rampMs = command.rampMs;
    event.periodFrames = period;
    if (!continuous) event.endFrameExclusive = demoEnd;
    if (event.paramId == 0 && !event.paramName.empty()) {
      const auto found = nodeTypes.find(event.nodeId);
      if (found != nodeTypes.end()) {
        const auto& type = found->second;
        if (type == "kick") event.paramId = resolveParamIdByName(kKickParamMap, event.paramName);
        else if (type == "clap") event.paramId = resolveParamIdByName(kClapParamMap, event.paramName);
        else if (type == "tb303_ext") event.paramId = resolveParamIdByName(kTb303ParamMap, event.paramName);
        else if (type == "mam_chip") event.paramId = resolveParamIdByName(kMamChipParamMap, event.paramName);
      }
    }
    result.events.push_back(std::move(event));
  }
  return result;
}

} // namespace mam::app
