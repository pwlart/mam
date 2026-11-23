#pragma once
#include "CliOptions.hpp"
#include <atomic>

namespace mam::app {
// Platform application boundary. Planning helpers below this layer need no device.
int runRealtimeSession(const CliOptions& options, std::atomic<bool>& running,
                       bool (*stdinReady)());
} // namespace mam::app
