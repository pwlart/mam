#include "CliOptions.hpp"
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstring>

namespace mam::app {

CliEntry selectCliEntry(int argc, const char* const argv[]) {
  if (argc == 2 && std::strcmp(argv[1], "--build-info") == 0) return CliEntry::BuildInfo;
  if (argc > 1 && std::strcmp(argv[1], "tools") == 0) return CliEntry::PythonTools;
  if (argc > 1 && std::strcmp(argv[1], "stems") == 0) return CliEntry::Stems;
  return CliEntry::Legacy;
}

void printCliUsage(const char* exe, std::FILE* diagnostics) {
  std::fprintf(diagnostics, "Build provenance: %s --build-info (JSON; no audio device)\n", exe);
  std::fprintf(diagnostics, "Offline tools: %s tools doctor | tools run JOB.json | stems render JOB.json\n", exe);
  std::fprintf(diagnostics,
               "Usage: %s [--f0 Hz] [--fend Hz] [--pitch-decay ms] [--amp-decay ms]\n"
               "          [--gain 0..1] [--bpm N] [--click 0..1]\n"
               "          [--wav path] [--sr Hz] [--pcm16] [--format wav|aiff|caf] [--bitdepth 16|24|32f] [--offline-threads N]\n"
               "          [--rack path.json] [--quit-after sec]\\n\n"
               "\nOffline export controls (auto-duration by default):\n"
               "  --duration SEC     Hard duration (overrides auto)\n"
               "  --bars N           Force N bars from transport (if present)\n"
               "  --loop-count N     Repeat transport sequence N times (default 1)\n"
               "  --tail-ms MS       Decay tail appended (default 250)\n"
               "  --normalize        Normalize to peak target (default -1.0 dBFS unless changed)\n"
               "  --peak-target dB   Peak target for normalization (e.g., -0.3)\n"
               "  --verbose          Print realtime loop diagnostics (loop counter and elapsed time)\n"
               "  --print-triggers   Print realtime event deliveries (set/ramps/triggers) at render-time\n"
               "  --dump-events      Print synthesized command events (time/bar/step) before playback/export\n"
               "  --meters           Realtime: print per-rack meters periodically; Offline: print mix meters after export\n"
               "  --meters-interval S  Realtime meters print interval in seconds (min 0.05, default 1.0)\n"
               "  --meters-per-node  Print per-node peak/RMS after run/export (realtime at loop boundaries; offline after export)\n"
               "                      Realtime also prints per-bus meters when buses are defined in session.\n"
               "  --cpu-stats        Print block CPU avg/max and xrun count at end\n"
               "  --cpu-stats-per-node  Print per-node avg/max us at end\n"
               "  --topo-metrics     Shortcut: enable both --cpu-stats and --cpu-stats-per-node for topo\n"
               "  --sha1             Print SHA1(samples) of rendered output (offline)\n"
               "  --rt-debug-feed   Debug realtime feeder (queue pushes, offsets)\n"
               "  --rt-debug-session Debug realtime session (initial/feeder enqueues)\n"
               "  --session path.json  Run a multi-rack session (realtime or offline when combined with --wav)\n"
               "\nDiagram export (Mermaid):\n"
               "  --export-mermaid-session path.json   Print session (racks/buses/routes) as Mermaid flowchart to stdout\n"
               "  --export-mermaid-graph path.json     Print graph (nodes/connections) as Mermaid flowchart to stdout\n"
               "  --loop-minutes M   Repeat transport to reach at least M minutes (offline)\n"
               "  --loop-seconds S   Repeat transport to reach at least S seconds (offline)\n"
               "  --random-seed N    Override graph/session rack seeds (decimal 0..4294967295; 0 = use JSON)\n"
               "  --topo-scheduler topo|baseline   Select topo offline scheduler (alias: --offline-scheduler)\n"
               "  --topo-offline-blocks N          Topo scheduler block size (alias: --offline-block)\n"
               "  --topo-threads N                 Level-parallel jobs (0=serial)\n"
               "  --topo-min-width N               Min level width to enable parallelism (default 2)\n"
               "  --topo-min-seg-frames N          Min segment frames to parallelize (default 128)\n"
               "  --topo-verbose                   Print topo levels/buffer reuse diagnostics\n"
               "          [--validate path.json] [--list-nodes path.json] [--list-params kick|clap] [--list-node-types]\n"
               "\nProgress (offline):\n"
               "  --progress-ms N    Progress print interval in ms (0=disable)\n"
               "  --no-progress      Disable progress prints\n"
               "  --no-summary       Disable final speedup summary\n"
               "\n"
               "Examples:\n"
               "  mam                       # one-shot, defaults (real-time)\n"
               "  mam --bpm 120            # 120 BPM continuous till Ctrl-C (real-time)\n"
               "  mam --rack demo.json --wav demo.wav         # export using auto-duration\n",
               exe);
}

std::optional<int> parseCliOptions(int argc, const char* const argv[], CliOptions& options,
                                  std::FILE* diagnostics) {
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    auto need = [&](int remain) {
      if (i + remain >= argc) {
        printCliUsage(argv[0], diagnostics);
        return false;
      }
      return true;
    };
    if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
      printCliUsage(argv[0], diagnostics);
      return 0;
    } else if (std::strcmp(a, "--f0") == 0) {
      if (!need(1)) return 1;
      options.params.startFreqHz = static_cast<float>(std::atof(argv[++i]));
    } else if (std::strcmp(a, "--fend") == 0) {
      if (!need(1)) return 1;
      options.params.endFreqHz = static_cast<float>(std::atof(argv[++i]));
    } else if (std::strcmp(a, "--pitch-decay") == 0) {
      if (!need(1)) return 1;
      options.params.pitchDecayMs = static_cast<float>(std::atof(argv[++i]));
    } else if (std::strcmp(a, "--amp-decay") == 0) {
      if (!need(1)) return 1;
      options.params.ampDecayMs = static_cast<float>(std::atof(argv[++i]));
    } else if (std::strcmp(a, "--gain") == 0) {
      if (!need(1)) return 1;
      options.params.gain = static_cast<float>(std::atof(argv[++i]));
    } else if (std::strcmp(a, "--bpm") == 0) {
      if (!need(1)) return 1;
      options.params.bpm = static_cast<float>(std::atof(argv[++i]));
    } else if (std::strcmp(a, "--duration") == 0) {
      if (!need(1)) return 1;
      options.overrideDurationSec = std::atof(argv[++i]);
    } else if (std::strcmp(a, "--click") == 0) {
      if (!need(1)) return 1;
      options.params.click = static_cast<float>(std::atof(argv[++i]));
    } else if (std::strcmp(a, "--wav") == 0) {
      // Optional filename: if omitted or next arg looks like a flag, auto-name later
      if (i + 1 >= argc || (argv[i + 1] && argv[i + 1][0] == '-')) {
        options.autoWavName = true;
      } else {
        options.wavPath = argv[++i];
      }
    } else if (std::strcmp(a, "--sr") == 0) {
      if (!need(1)) return 1;
      options.offlineSr = std::atof(argv[++i]);
      if (options.offlineSr <= 8000.0) options.offlineSr = 8000.0;
    } else if (std::strcmp(a, "--format") == 0) {
      if (!need(1)) return 1;
      {
        const char* f = argv[++i];
        if (std::strcmp(f, "wav") == 0) options.outFormat = FileFormat::Wav;
        else if (std::strcmp(f, "aiff") == 0) options.outFormat = FileFormat::Aiff;
        else if (std::strcmp(f, "caf") == 0) options.outFormat = FileFormat::Caf;
      }
    } else if (std::strcmp(a, "--bitdepth") == 0) {
      if (!need(1)) return 1;
      {
        const char* b = argv[++i];
        if (std::strcmp(b, "16") == 0) options.outDepth = BitDepth::Pcm16;
        else if (std::strcmp(b, "24") == 0) options.outDepth = BitDepth::Pcm24;
        else options.outDepth = BitDepth::Float32;
      }
    } else if (std::strcmp(a, "--pcm16") == 0) {
      options.pcm16 = true;
    } else if (std::strcmp(a, "--offline-threads") == 0) {
      if (!need(1)) return 1;
      options.offlineThreads = static_cast<uint32_t>(std::max(0, std::atoi(argv[++i])));
    } else if (std::strcmp(a, "--quit-after") == 0) {
      if (!need(1)) return 1;
      options.quitAfterSec = std::atof(argv[++i]);
    } else if (std::strcmp(a, "--bars") == 0) {
      if (!need(1)) return 1;
      options.overrideBars = static_cast<uint32_t>(std::max(0, std::atoi(argv[++i])));
    } else if (std::strcmp(a, "--loop-count") == 0) {
      if (!need(1)) return 1;
      options.overrideLoopCount = static_cast<uint32_t>(std::max(0, std::atoi(argv[++i])));
    } else if (std::strcmp(a, "--start-bar") == 0) {
      if (!need(1)) return 1;
      options.overrideStartBar = static_cast<uint32_t>(std::max(0, std::atoi(argv[++i])));
    } else if (std::strcmp(a, "--end-bar") == 0) {
      if (!need(1)) return 1;
      options.overrideEndBar = static_cast<uint32_t>(std::max(0, std::atoi(argv[++i])));
    } else if (std::strcmp(a, "--loop-minutes") == 0) {
      if (!need(1)) return 1;
      options.loopMinutes = std::max(0.0, std::atof(argv[++i]));
    } else if (std::strcmp(a, "--loop-seconds") == 0) {
      if (!need(1)) return 1;
      options.loopSeconds = std::max(0.0, std::atof(argv[++i]));
    } else if (std::strcmp(a, "--tail-ms") == 0) {
      if (!need(1)) return 1;
      options.tailMs = std::max(0.0, std::atof(argv[++i])); options.tailOverridden = true;
    } else if (std::strcmp(a, "--normalize") == 0) {
      options.doNormalize = true; options.peakTargetDb = -1.0;
    } else if (std::strcmp(a, "--peak-target") == 0) {
      if (!need(1)) return 1;
      options.doNormalize = true; options.peakTargetDb = std::atof(argv[++i]);
    } else if (std::strcmp(a, "--verbose") == 0 || std::strcmp(a, "-v") == 0) {
      options.verbose = true;
    } else if (std::strcmp(a, "--rack") == 0) {
      if (!need(1)) return 1;
      options.rackPath = argv[++i];
    } else if (std::strcmp(a, "--session") == 0) {
      if (!need(1)) return 1;
      options.sessionPath = argv[++i];
    } else if (std::strcmp(a, "--metrics-ndjson") == 0) {
      if (!need(1)) return 1;
      options.metricsNdjsonPath = argv[++i];
    } else if (std::strcmp(a, "--metrics-scope") == 0) {
      if (!need(1)) return 1;
      {
        std::string sc = argv[++i];
        options.metricsScopeRacks = (sc.find("racks") != std::string::npos);
        options.metricsScopeBuses = (sc.find("buses") != std::string::npos);
      }
    } else if (std::strcmp(a, "--export-mermaid-session") == 0) {
      if (!need(1)) return 1;
      options.exportMermaidSessionPath = argv[++i];
    } else if (std::strcmp(a, "--export-mermaid-graph") == 0) {
      if (!need(1)) return 1;
      options.exportMermaidGraphPath = argv[++i];
    } else if (std::strcmp(a, "--print-topo") == 0) {
      options.printTopo = true;
    } else if (std::strcmp(a, "--print-ports") == 0) {
      options.printPorts = true;
    } else if (std::strcmp(a, "--meters") == 0) {
      options.printMeters = true;
    } else if (std::strcmp(a, "--offline-scheduler") == 0) {
      if (!need(1)) return 1;
      options.offlineScheduler = argv[++i];
    } else if (std::strcmp(a, "--offline-block") == 0) {
      if (!need(1)) return 1;
      options.offlineBlock = static_cast<uint32_t>(std::max(64, std::atoi(argv[++i])));
    } else if (std::strcmp(a, "--topo-scheduler") == 0) {
      if (!need(1)) return 1;
      options.offlineScheduler = argv[++i];
    } else if (std::strcmp(a, "--topo-offline-blocks") == 0) {
      if (!need(1)) return 1;
      options.offlineBlock = static_cast<uint32_t>(std::max(64, std::atoi(argv[++i])));
    } else if (std::strcmp(a, "--topo-verbose") == 0) {
      options.topoVerbose = true;
    } else if (std::strcmp(a, "--topo-threads") == 0) {
      if (!need(1)) return 1;
      options.topoThreads = static_cast<uint32_t>(std::max(0, std::atoi(argv[++i])));
    } else if (std::strcmp(a, "--topo-min-width") == 0) {
      if (!need(1)) return 1;
      options.topoMinWidth = static_cast<uint32_t>(std::max(1, std::atoi(argv[++i])));
    } else if (std::strcmp(a, "--topo-min-seg-frames") == 0) {
      if (!need(1)) return 1;
      options.topoMinSegFrames = static_cast<uint32_t>(std::max(1, std::atoi(argv[++i])));
    } else if (std::strcmp(a, "--topo-trace") == 0) {
      if (!need(1)) return 1;
      options.topoTracePath = argv[++i];
    } else if (std::strcmp(a, "--meters-interval") == 0) {
      if (!need(1)) return 1;
      options.metersIntervalSec = std::atof(argv[++i]);
      if (!(options.metersIntervalSec > 0.05)) options.metersIntervalSec = 1.0; // clamp to sane default
    } else if (std::strcmp(a, "--meters-per-node") == 0) {
      options.metersPerNode = true;
    } else if (std::strcmp(a, "--cpu-stats") == 0) {
      options.cpuStats = true;
    } else if (std::strcmp(a, "--cpu-stats-per-node") == 0) {
      options.cpuStatsPerNode = true;
    } else if (std::strcmp(a, "--topo-metrics") == 0) {
      options.cpuStats = true; options.cpuStatsPerNode = true;
    } else if (std::strcmp(a, "--rt-debug-feed") == 0) {
      options.rtDebugFeed = true;
    } else if (std::strcmp(a, "--rt-debug-session") == 0) {
      options.rtDebugSession = true;
    } else if (std::strcmp(a, "--random-seed") == 0) {
      if (!need(1)) return 1;
      const char* first = argv[++i];
      const char* last = first + std::strlen(first);
      uint32_t seed = 0;
      const auto parsed = std::from_chars(first, last, seed, 10);
      if (parsed.ec != std::errc{} || parsed.ptr != last) {
        std::fprintf(diagnostics, "Invalid --random-seed: expected a decimal integer in 0..4294967295\n");
        return 1;
      }
      options.randomSeedOverride = seed;
    } else if (std::strcmp(a, "--progress-ms") == 0) {
      if (!need(1)) return 1;
      options.offlineProgressMs = std::atoi(argv[++i]);
    } else if (std::strcmp(a, "--no-progress") == 0) {
      options.offlineProgressEnabled = false;
    } else if (std::strcmp(a, "--no-summary") == 0) {
      options.offlineSummaryEnabled = false;
    } else if (std::strcmp(a, "--print-triggers") == 0) {
      options.printTriggers = true;
    } else if (std::strcmp(a, "--dump-events") == 0) {
      options.dumpEvents = true;
    } else if (std::strcmp(a, "--print-latency") == 0) {
      options.printLatency = true;
    } else if (std::strcmp(a, "--schema-strict") == 0) {
      options.schemaStrict = true;
    } else if (std::strcmp(a, "--trace-json") == 0) {
      if (!need(1)) return 1;
      options.traceJsonPath = argv[++i];
    } else if (std::strcmp(a, "--sha1") == 0) {
      options.printSha1 = true;
    } else if (std::strcmp(a, "--validate") == 0) {
      if (!need(1)) return 1;
      options.validatePath = argv[++i];
    } else if (std::strcmp(a, "--list-nodes") == 0) {
      if (!need(1)) return 1;
      options.listNodesPath = argv[++i];
    } else if (std::strcmp(a, "--list-params") == 0) {
      if (!need(1)) return 1;
      options.listParamsType = argv[++i];
    } else if (std::strcmp(a, "--list-node-types") == 0) {
      options.listNodeTypes = true;
    } else {
      std::fprintf(diagnostics, "Unknown option: %s\n", a);
      printCliUsage(argv[0], diagnostics);
      return 1;
    }
  }

  return std::nullopt;
}

} // namespace mam::app
