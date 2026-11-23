#pragma once

#include "../core/GraphConfig.hpp"
#include <cstdint>
#include <vector>

// Preparation-side command generation. This compiled boundary deliberately
// preserves legacy rounding and ordering until all transport consumers migrate.
std::vector<GraphSpec::CommandSpec> generateCommandsFromTransport(
    const GraphSpec::Transport& transport, uint32_t sampleRate);
