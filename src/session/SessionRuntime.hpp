#pragma once

#include "SessionSpec.hpp"
#include "../core/Graph.hpp"
#include "../core/GraphConfig.hpp"
#include <cstdint>
#include <string>
#include <vector>

struct SessionRuntime {
  struct Rack {
    std::string id;
    Graph graph;
    GraphSpec spec;
    std::vector<GraphSpec::CommandSpec> cmds;
    int64_t startOffsetFrames = 0;
    float gain = 1.0f;
    bool muted = false;
    bool solo = false;
  };

  struct RackStats {
    std::string id;
    double peakDb = 0.0;
    double rmsDb = 0.0;
    double cpuAvgMs = 0.0;
    double cpuMaxMs = 0.0;
    uint64_t blocks = 0;
  };

  uint32_t sampleRate = 48000;
  uint32_t channels = 2;
  std::vector<Rack> racks;
  bool enablePerRackMeters = false;
  bool enablePerRackCpu = false;
  struct Bus { std::string id; uint32_t channels = 2; std::vector<float> buffer; std::vector<SessionSpec::InsertRef> inserts; };
  std::vector<Bus> buses;
  struct Route { std::string from; std::string to; float gain = 1.0f; };
  std::vector<Route> routes;
  // Session-level commands (resolved to sample time)
  std::vector<GraphSpec::CommandSpec> sessionCommands;

  void setPerRackMeters(bool v) { enablePerRackMeters = v; }
  void setPerRackCpu(bool v) { enablePerRackCpu = v; }

  static void computePeakAndRmsSimple(const std::vector<float>& interleaved, uint32_t /*channels*/, double& outPeakDb, double& outRmsDb);

  void loadFromSpec(const SessionSpec& s, uint32_t seedOverride = 0);

  // Render offline: mix simple sum with per-rack gain and start offset (silence before start)
  std::vector<float> renderOffline(uint64_t frames, std::vector<RackStats>* outStats = nullptr);

  // Plan total frames considering content length, preroll, start offsets, and looping
  uint64_t planTotalFrames(double sessionTailMs, bool enableLoop = false, uint32_t maxLoops = 1) const;

  // Partition frames across maxLoops; earliest loops receive remainder frames.
  // Concatenate reset renders, with no extra tail; stats describe the last
  // nonempty loop. Zero loops return silence. Invalid channels/storage throw.
  std::vector<float> renderOfflineWithLoop(uint64_t frames, uint32_t maxLoops = 1, std::vector<RackStats>* outStats = nullptr);
};
