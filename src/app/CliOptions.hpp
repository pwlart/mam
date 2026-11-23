#pragma once

#include "../instruments/kick/KickSynth.hpp"
#include "../io/AudioFileWriter.hpp"
#include <cstdio>
#include <optional>
#include <string>

namespace mam::app {

// Parsing owns values only. Engine/device setup and global controls are applied
// by the application after parsing, so tests never open an audio device.
struct CliOptions {
  KickParams params;
  std::string wavPath;
  FileFormat outFormat = FileFormat::Wav;
  BitDepth outDepth = BitDepth::Float32;
  std::string graphPath;
  std::string rackPath; // preferred alias for graphPath
  std::string sessionPath;
  std::string validatePath;
  std::string listNodesPath;
  std::string listParamsType;
  bool listNodeTypes = false;
  double offlineSr = 48000.0;
  bool pcm16 = false;
  double quitAfterSec = 0.0;
  uint32_t offlineThreads = 0; // 0=auto (fallback to single-thread renderer if 0)
  bool autoWavName = false;     // if --wav provided without filename, auto-name output
  // Export behavior controls
  double overrideDurationSec = -1.0; // < 0 means auto
  uint32_t overrideBars = 0;         // 0 means use transport length
  uint32_t overrideLoopCount = 0;    // 0 means single pass
  uint32_t overrideStartBar = 0;     // start bar (0-based), 0 means beginning
  uint32_t overrideEndBar = 0;       // end bar (exclusive), 0 means transport length
  double loopMinutes = 0.0;          // derive loop-count if > 0
  double loopSeconds = 0.0;          // derive loop-count if > 0
  double tailMs = 250.0;             // default decay tail
  bool doNormalize = false;          // normalize to peak target if true
  double peakTargetDb = -1.0;        // default peak target when normalizing
  bool printTopo = false;            // print topo order from connections
  bool printPorts = false;           // print declared ports per node
  bool printMeters = false;          // print meter summary explicitly
  double metersIntervalSec = 1.0;    // realtime meters print interval
  bool tailOverridden = false;
  bool verbose = false;              // realtime loop diagnostics
  bool topoVerbose = false;          // topo scheduler debug prints
  std::string offlineScheduler = "baseline"; // "baseline" | "topo"
  uint32_t offlineBlock = 1024;      // offline block size
  uint32_t topoThreads = 0;          // 0=serial; >0 level-parallel jobs
  uint32_t topoMinWidth = 2;         // min nodes in level to parallelize
  uint32_t topoMinSegFrames = 128;   // min segment frames to parallelize
  std::string topoTracePath;         // optional Chrome trace path
  uint32_t randomSeedOverride = 0;   // override JSON randomSeed if non-zero
  bool metersPerNode = false;
  std::string metricsNdjsonPath;
  bool metricsScopeRacks = true;
  bool metricsScopeBuses = true; // nodes later
  std::string exportMermaidSessionPath;
  std::string exportMermaidGraphPath;
  std::string traceJsonPath;
  bool printSha1 = false;
  bool cpuStats = false;
  bool cpuStatsPerNode = false;
  bool rtDebugFeed = false;
  bool rtDebugSession = false;
  bool printTriggers = false;
  bool dumpEvents = false;
  bool schemaStrict = false;         // enforce JSON Schema on load
  bool printLatency = false;         // print preroll/latency info
  int offlineProgressMs = 100;
  bool offlineProgressEnabled = true;
  bool offlineSummaryEnabled = true;
};

enum class CliEntry { Legacy, BuildInfo, PythonTools, Stems };
CliEntry selectCliEntry(int argc, const char* const argv[]);
void printCliUsage(const char* executable, std::FILE* diagnostics = stderr);

// nullopt means continue; a value is the help/error exit code for the caller.
// Uses the historical conversions/clamps intentionally; stricter validation is
// a separate behavior change, not part of the extraction.
std::optional<int> parseCliOptions(int argc, const char* const argv[], CliOptions& options,
                                  std::FILE* diagnostics = stderr);

} // namespace mam::app
