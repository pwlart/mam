#pragma once

#include "SessionSpec.hpp"
#include "../core/GraphConfig.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace mam::session {

// Preparation-side, owning representation of one rack's legacy event sequence.
// This preserves current transport rounding and explicit-before-generated order;
// global queue ordering and repetition belong to the session scheduler.
struct RackTimeline {
  std::string rackId;
  std::vector<GraphSpec::CommandSpec> baseCmds;
  uint64_t loopLen = 0;
  uint64_t framesPerBar = 0;
};

// `active` is the session's already-resolved solo/mute policy. No files, devices
// or shared state are touched. Inputs and their unprefixed IDs remain unchanged.
RackTimeline planRackTimeline(const GraphSpec& graph, const SessionSpec::RackRef& rack,
                              uint32_t sampleRate, bool active);

} // namespace mam::session
