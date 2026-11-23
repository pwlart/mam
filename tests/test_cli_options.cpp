#include "app/CliOptions.hpp"
#include "offline/OfflineProgress.hpp"
#include <doctest/doctest.h>
#include <cstdio>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
struct Parsed {
  mam::app::CliOptions options;
  std::optional<int> exitCode;
  std::string diagnostics;
};

Parsed parse(std::initializer_list<const char*> arguments) {
  std::vector<const char*> argv{"mam"};
  argv.insert(argv.end(), arguments.begin(), arguments.end());
  std::unique_ptr<std::FILE, decltype(&std::fclose)> output(std::tmpfile(), &std::fclose);
  if (!output) throw std::runtime_error("Cannot create private diagnostics fixture");
  Parsed result;
  result.exitCode = mam::app::parseCliOptions(static_cast<int>(argv.size()), argv.data(), result.options, output.get());
  std::rewind(output.get());
  char buffer[1024];
  while (const auto count = std::fread(buffer, 1, sizeof(buffer), output.get())) {
    result.diagnostics.append(buffer, count);
  }
  return result;
}
} // namespace

TEST_CASE("CLI defaults preserve the legacy render configuration") {
  const auto parsed = parse({});
  CHECK_FALSE(parsed.exitCode.has_value());
  CHECK(parsed.diagnostics.empty());
  const auto& o = parsed.options;
  CHECK(o.wavPath.empty());
  CHECK(o.sessionPath.empty());
  CHECK(o.params.startFreqHz == doctest::Approx(100));
  CHECK(o.params.endFreqHz == doctest::Approx(40));
  CHECK(o.params.gain == doctest::Approx(.9));
  CHECK_FALSE(o.params.loop);
  CHECK(o.offlineSr == 48000);
  CHECK(o.outFormat == FileFormat::Wav);
  CHECK(o.outDepth == BitDepth::Float32);
  CHECK(o.offlineScheduler == "baseline");
  CHECK(o.offlineBlock == 1024);
  CHECK(o.overrideDurationSec == -1);
  CHECK(o.tailMs == 250);
  CHECK(o.offlineProgressMs == 100);
  CHECK(o.offlineProgressEnabled);
  CHECK(o.offlineSummaryEnabled);
}

TEST_CASE("CLI preserves session export and diagnostic choices without loading files") {
  const auto parsed = parse({"--session", "missing session.json", "--wav", "new mix.wav", "--sr", "44100",
      "--format", "caf", "--bitdepth", "24", "--duration", "12", "--tail-ms", "75",
      "--bars", "8", "--start-bar", "2", "--end-bar", "6", "--loop-count", "3",
      "--loop-minutes", "2.5", "--loop-seconds", "7", "--quit-after", "3.5",
      "--metrics-ndjson", "meters.ndjson", "--metrics-scope", "buses", "--print-triggers",
      "--rt-debug-session", "--dump-events", "--print-latency", "--schema-strict", "--sha1"});
  REQUIRE_FALSE(parsed.exitCode.has_value());
  CHECK(parsed.diagnostics.empty());
  const auto& o = parsed.options;
  CHECK(o.sessionPath == "missing session.json");
  CHECK(o.wavPath == "new mix.wav");
  CHECK(o.offlineSr == 44100);
  CHECK(o.outFormat == FileFormat::Caf);
  CHECK(o.outDepth == BitDepth::Pcm24);
  CHECK(o.overrideDurationSec == 12);
  CHECK(o.tailMs == 75);
  CHECK(o.tailOverridden);
  CHECK(o.overrideBars == 8);
  CHECK(o.overrideStartBar == 2);
  CHECK(o.overrideEndBar == 6);
  CHECK(o.overrideLoopCount == 3);
  CHECK(o.loopMinutes == 2.5);
  CHECK(o.loopSeconds == 7);
  CHECK(o.quitAfterSec == 3.5);
  CHECK(o.metricsNdjsonPath == "meters.ndjson");
  CHECK_FALSE(o.metricsScopeRacks);
  CHECK(o.metricsScopeBuses);
  CHECK(o.printTriggers);
  CHECK(o.rtDebugSession);
  CHECK(o.dumpEvents);
  CHECK(o.printLatency);
  CHECK(o.schemaStrict);
  CHECK(o.printSha1);
}

TEST_CASE("CLI optional WAV names preserve offline dispatch intent") {
  const auto bare = parse({"--wav"});
  REQUIRE_FALSE(bare.exitCode.has_value());
  CHECK(bare.options.autoWavName);
  CHECK(bare.options.wavPath.empty()); // the app supplies its historical placeholder later
  const auto followed = parse({"--wav", "--sr", "44100"});
  REQUIRE_FALSE(followed.exitCode.has_value());
  CHECK(followed.options.autoWavName);
  CHECK(followed.options.offlineSr == 44100);
  const auto named = parse({"--wav", "result.wav"});
  REQUIRE_FALSE(named.exitCode.has_value());
  CHECK_FALSE(named.options.autoWavName);
  CHECK(named.options.wavPath == "result.wav");
}

TEST_CASE("CLI aliases clamps and option ordering retain existing semantics") {
  const auto oldAliases = parse({"--offline-scheduler", "topo", "--offline-block", "32", "--sr", "1000",
      "--offline-threads", "-4", "--meters-interval", "0.05", "--peak-target", "-3", "--normalize"});
  const auto topoAliases = parse({"--topo-scheduler", "topo", "--topo-offline-blocks", "32",
      "--normalize", "--peak-target", "-3", "--topo-threads", "4", "--topo-min-width", "0",
      "--topo-min-seg-frames", "0", "--topo-metrics", "--topo-trace", "trace.json"});
  REQUIRE_FALSE(oldAliases.exitCode.has_value());
  REQUIRE_FALSE(topoAliases.exitCode.has_value());
  CHECK(oldAliases.options.offlineScheduler == topoAliases.options.offlineScheduler);
  CHECK(oldAliases.options.offlineBlock == 64);
  CHECK(topoAliases.options.offlineBlock == 64);
  CHECK(oldAliases.options.offlineSr == 8000);
  CHECK(oldAliases.options.offlineThreads == 0);
  CHECK(oldAliases.options.metersIntervalSec == 1);
  CHECK(oldAliases.options.doNormalize);
  CHECK(oldAliases.options.peakTargetDb == -1);
  CHECK(topoAliases.options.peakTargetDb == -3);
  CHECK(topoAliases.options.topoThreads == 4);
  CHECK(topoAliases.options.topoMinWidth == 1);
  CHECK(topoAliases.options.topoMinSegFrames == 1);
  CHECK(topoAliases.options.cpuStats);
  CHECK(topoAliases.options.cpuStatsPerNode);
  CHECK(topoAliases.options.topoTracePath == "trace.json");
}

TEST_CASE("CLI missing required values return failure without exiting the caller") {
  for (const auto* flag : {"--f0", "--fend", "--pitch-decay", "--amp-decay", "--gain", "--bpm",
      "--duration", "--click", "--sr", "--format", "--bitdepth", "--offline-threads", "--quit-after",
      "--bars", "--loop-count", "--start-bar", "--end-bar", "--loop-minutes", "--loop-seconds",
      "--tail-ms", "--peak-target", "--rack", "--session", "--metrics-ndjson", "--metrics-scope",
      "--export-mermaid-session", "--export-mermaid-graph", "--offline-scheduler", "--offline-block",
      "--topo-scheduler", "--topo-offline-blocks", "--topo-threads", "--topo-min-width",
      "--topo-min-seg-frames", "--topo-trace", "--meters-interval", "--random-seed", "--progress-ms",
      "--trace-json", "--validate", "--list-nodes", "--list-params"}) {
    INFO(flag);
    const auto parsed = parse({flag});
    REQUIRE(parsed.exitCode.has_value());
    CHECK(*parsed.exitCode == 1);
    CHECK(parsed.diagnostics.find("Usage: mam ") != std::string::npos);
  }
}

TEST_CASE("CLI help and unknown options keep their distinct exit codes") {
  for (const auto* flag : {"--help", "-h"}) {
    const auto parsed = parse({flag, "--unknown-after-help"});
    REQUIRE(parsed.exitCode.has_value());
    CHECK(*parsed.exitCode == 0);
    CHECK(parsed.diagnostics.find("Usage: mam ") != std::string::npos);
    CHECK(parsed.diagnostics.find("Unknown option") == std::string::npos);
  }
  const auto bad = parse({"--unknown-option", "--help"});
  REQUIRE(bad.exitCode.has_value());
  CHECK(*bad.exitCode == 1);
  CHECK(bad.diagnostics.find("Unknown option: --unknown-option\n") == 0);
  CHECK(bad.diagnostics.find("Usage: mam ") != std::string::npos);
}

TEST_CASE("CLI early dispatch keeps build info and worker arguments outside legacy parsing") {
  const auto select = [](std::initializer_list<const char*> arguments) {
    std::vector<const char*> argv{"mam"};
    argv.insert(argv.end(), arguments.begin(), arguments.end());
    return mam::app::selectCliEntry(static_cast<int>(argv.size()), argv.data());
  };
  using Entry = mam::app::CliEntry;
  CHECK(select({"--build-info"}) == Entry::BuildInfo);
  CHECK(select({"--build-info", "--help"}) == Entry::Legacy);
  CHECK(select({"tools", "--python", "python path", "run", "job.json"}) == Entry::PythonTools);
  CHECK(select({"stems", "render", "job.json"}) == Entry::Stems);
  CHECK(select({"tools"}) == Entry::PythonTools);
  CHECK(select({"stems"}) == Entry::Stems);
  CHECK(select({"--session", "session.json", "--wav"}) == Entry::Legacy);
  CHECK(select({"--help"}) == Entry::Legacy);
  CHECK(select({}) == Entry::Legacy);
}

TEST_CASE("CLI parsing records progress and seed settings without changing runtime globals") {
  const int progress = gOfflineProgressMs;
  const bool enabled = gOfflineProgressEnabled;
  const bool summary = gOfflineSummaryEnabled;
  const auto parsed = parse({"--progress-ms", "50", "--no-progress", "--no-summary", "--random-seed", "12"});
  REQUIRE_FALSE(parsed.exitCode.has_value());
  CHECK(parsed.options.offlineProgressMs == 50);
  CHECK_FALSE(parsed.options.offlineProgressEnabled);
  CHECK_FALSE(parsed.options.offlineSummaryEnabled);
  CHECK(parsed.options.randomSeedOverride == 12);
  CHECK(gOfflineProgressMs == progress);
  CHECK(gOfflineProgressEnabled == enabled);
  CHECK(gOfflineSummaryEnabled == summary);
}

TEST_CASE("CLI random seed accepts the complete uint32 decimal range") {
  for (const auto& [text, expected] : std::vector<std::pair<const char*, uint32_t>>{
      {"0", 0u}, {"1", 1u}, {"00012", 12u}, {"2147483648", 2147483648u}, {"4294967295", 4294967295u}}) {
    INFO(text);
    const auto parsed = parse({"--random-seed", text});
    REQUIRE_FALSE(parsed.exitCode.has_value());
    CHECK(parsed.diagnostics.empty());
    CHECK(parsed.options.randomSeedOverride == expected);
  }
  const auto unspecified = parse({"--random-seed", "12", "--random-seed", "0"});
  REQUIRE_FALSE(unspecified.exitCode.has_value());
  CHECK(unspecified.options.randomSeedOverride == 0);
}

TEST_CASE("CLI random seed rejects malformed and overflowing arguments without coercion") {
  for (const auto* text : {"", "-1", "-0", "+1", " 1", "1 ", "\t1", "1\n", "1.0", "1e2",
      "0x10", "12trailing", "--wav", "true", "4294967296", "18446744073709551615",
      "99999999999999999999999999999999999999"}) {
    INFO(text);
    const auto parsed = parse({"--random-seed", "12", "--random-seed", text});
    REQUIRE(parsed.exitCode.has_value());
    CHECK(*parsed.exitCode == 1);
    CHECK(parsed.diagnostics == "Invalid --random-seed: expected a decimal integer in 0..4294967295\n");
    CHECK(parsed.options.randomSeedOverride == 12);
  }
}
