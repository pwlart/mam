#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudioTypes.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <csignal>
#include <thread>
#include <chrono>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include <queue>
#include <memory>
#include <iostream>
#include <unistd.h>
#include <sys/select.h>
#include <cassert>
#include <sstream>
#include "instruments/kick/KickSynth.hpp"
#include "offline/OfflineRenderer.hpp"
#include "offline/OfflineGraphRenderer.hpp"
#include "offline/OfflineParallelGraphRenderer.hpp"
#include "session/SessionSpec.hpp"
#include "session/SessionRuntime.hpp"
#include "offline/OfflineTimelineRenderer.hpp"
#include "offline/TransportGenerator.hpp"
#include "io/AudioFileWriter.hpp"
#include "offline/OfflineProgress.hpp"
#include "offline/OfflineTopoScheduler.hpp"
#include "realtime/RealtimeRenderer.hpp"
#include "realtime/RealtimeGraphRenderer.hpp"
#include "realtime/RealtimeSessionRenderer.hpp"
#include <fstream>
#include "core/Graph.hpp"
#include "core/GraphConfig.hpp"
#include "core/NodeFactory.hpp"
#include "core/MixerNode.hpp"
#include "instruments/kick/KickNode.hpp"
#include "core/ParamMap.hpp"
#include "core/Random.hpp"
#include "core/GraphUtils.hpp"
#include "core/SchemaValidate.hpp"
#include "core/Sha1.hpp"
#include "tools/PythonTools.hpp"
#include "tools/BuildInfo.hpp"
#include "app/CliOptions.hpp"
#include "app/SessionPlayback.hpp"
#include "app/GraphPlayback.hpp"
#include "session/SessionFeeding.hpp"
#include "offline/StemRenderer.hpp"

// The shared instrument implementation serves realtime and offline paths.

// Realtime callback now lives in RealtimeRenderer

static std::atomic<bool> gRunning{true};

static void onSigInt(int) {
  gRunning.store(false);
}

// non-blocking stdin check using select
static bool isStdinReady() {
  fd_set readfds;
  FD_ZERO(&readfds);
  FD_SET(STDIN_FILENO, &readfds);
  timeval tv{0, 0};
  int rv = select(STDIN_FILENO + 1, &readfds, nullptr, nullptr, &tv);
  return (rv > 0) && FD_ISSET(STDIN_FILENO, &readfds);
}

// printTopoOrderFromSpec and computeGraphPrerollSamples moved to core/GraphUtils.hpp

static std::string formatDuration(double seconds) {
  if (seconds < 0.0) seconds = 0.0;
  const int64_t totalMs = static_cast<int64_t>(seconds * 1000.0 + 0.5);
  const int64_t hrs = totalMs / (3600 * 1000);
  const int64_t rem1 = totalMs % (3600 * 1000);
  const int64_t mins = rem1 / (60 * 1000);
  const int64_t rem2 = rem1 % (60 * 1000);
  const int64_t secs = rem2 / 1000;
  const int64_t ms = rem2 % 1000;
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld.%03lld",
                static_cast<long long>(hrs), static_cast<long long>(mins),
                static_cast<long long>(secs), static_cast<long long>(ms));
  return std::string(buf);
}

static void computePeakAndRms(const std::vector<float>& interleaved, uint32_t /*channels*/, double& outPeakDb, double& outRmsDb) {
  double peak = 0.0;
  long double sumSq = 0.0L;
  const size_t n = interleaved.size();
  for (size_t i = 0; i < n; ++i) {
    const double s = static_cast<double>(interleaved[i]);
    const double a = std::fabs(s);
    if (a > peak) peak = a;
    sumSq += static_cast<long double>(s * s);
  }
  const double rms = (n > 0) ? std::sqrt(static_cast<double>(sumSq / static_cast<long double>(n))) : 0.0;
  auto toDb = [](double x) -> double { return (x > 0.0) ? (20.0 * std::log10(x)) : -std::numeric_limits<double>::infinity(); };
  outPeakDb = toDb(peak);
  outRmsDb = toDb(rms);
}

static const char* toStr(FileFormat f) {
  switch (f) {
    case FileFormat::Wav: return "wav";
    case FileFormat::Aiff: return "aiff";
    case FileFormat::Caf: return "caf";
  }
  return "wav";
}

static const char* toStr(BitDepth d) {
  switch (d) {
    case BitDepth::Pcm16: return "16";
    case BitDepth::Pcm24: return "24";
    case BitDepth::Float32: return "32f";
  }
  return "32f";
}

static std::string generateMermaidForSession(const SessionSpec& s) {
  std::ostringstream out;
  out << "flowchart LR\n";
  // Declare rack nodes
  for (const auto& r : s.racks) {
    out << "  " << r.id << "[\"Rack " << r.id << "\"]\n";
  }
  // Declare bus nodes (if any)
  for (const auto& b : s.buses) {
    out << "  " << b.id << "[(\"Bus " << b.id << "\")]\n";
  }
  // Declare master mix sink
  out << "  Mix[(\"Master Mix\")]\n";
  // Routes from racks to buses
  std::unordered_set<std::string> racksWithRoute;
  for (const auto& rt : s.routes) {
    racksWithRoute.insert(rt.from);
    out << "  " << rt.from << " -- \"gain=" << rt.gain << "\" --> " << rt.to << "\n";
  }
  // Any rack without a route goes directly to Mix
  for (const auto& r : s.racks) {
    if (racksWithRoute.find(r.id) == racksWithRoute.end()) {
      out << "  " << r.id << " --> Mix\n";
    }
  }
  // Buses go to Mix
  for (const auto& b : s.buses) {
    out << "  " << b.id << " --> Mix\n";
  }
  return out.str();
}

// Generate a simple Mermaid flowchart for a GraphSpec (nodes/connections)
static std::string generateMermaidForGraph(const GraphSpec& g) {
  std::ostringstream out;
  out << "flowchart LR\n";
  // Nodes
  for (const auto& n : g.nodes) {
    out << "  " << n.id << "[\"" << n.type << ": " << n.id << "\"]\n";
  }
  // Connections
  for (const auto& c : g.connections) {
    out << "  " << c.from << " -- \"g=" << c.gainPercent;
    if (c.fromPort != 0 || c.toPort != 0) {
      out << ", p" << c.fromPort << "->" << c.toPort;
    }
    if (c.dryPercent > 0.0f) {
      out << ", dry=" << c.dryPercent;
    }
    out << "\" --> " << c.to << "\n";
  }
  // Mixer (if any) — show inputs to Mix
  if (g.hasMixer) {
    out << "  Mix[(\"Graph Mix\")]\n";
    for (const auto& mi : g.mixer.inputs) {
      out << "  " << mi.id << " -- \"g=" << mi.gainPercent << "\" --> Mix\n";
    }
  }
  return out.str();
}


// Connectivity advisories (non-fatal): e.g., compressor without sidechain key
static void warnSidechainConnectivity(const GraphSpec& spec) {
  std::unordered_map<std::string, std::string> nodeType;
  nodeType.reserve(spec.nodes.size());
  for (const auto& n : spec.nodes) nodeType.emplace(n.id, n.type);
  std::unordered_map<std::string, bool> compHasKey;
  for (const auto& n : spec.nodes) if (n.type == std::string("compressor")) compHasKey[n.id] = false;
  for (const auto& c : spec.connections) {
    auto itT = nodeType.find(c.to);
    if (itT != nodeType.end() && itT->second == std::string("compressor") && c.toPort == 1u) {
      compHasKey[c.to] = true;
    }
  }
  for (const auto& kv : compHasKey) {
    if (!kv.second) {
      std::fprintf(stderr, "Warning: compressor '%s' has no sidechain key connected (toPort=1); using self-detection.\n", kv.first.c_str());
    }
  }
}


// Advisory: when a source has non-zero dryPercent taps and is also present in the mixer inputs,
// the engine suppresses those dry taps to prevent double-count. Print an authoring hint.
static void warnDrySuppression(const GraphSpec& spec) {
  if (!spec.hasMixer || spec.mixer.inputs.empty() || spec.connections.empty()) return;
  std::unordered_set<std::string> mixerIds;
  mixerIds.reserve(spec.mixer.inputs.size());
  for (const auto& mi : spec.mixer.inputs) mixerIds.insert(mi.id);
  std::unordered_set<std::string> warned;
  for (const auto& c : spec.connections) {
    if (c.dryPercent > 0.0f && mixerIds.count(c.from) && !warned.count(c.from)) {
      std::fprintf(stderr, "Info: dry tap(s) from '%s' will be suppressed because it is in the mixer inputs. Prefer either mixer input or dry tap, not both.\n", c.from.c_str());
      warned.insert(c.from);
    }
  }
}


static int listNodesGraphJson(const std::string& path) {
  try {
    GraphSpec spec = loadGraphSpecFromJsonFile(path);
    std::printf("Nodes (%zu):\n", spec.nodes.size());
    for (const auto& n : spec.nodes) {
      std::printf("- id=%s type=%s\n", n.id.c_str(), n.type.c_str());
    }
    if (spec.hasMixer) {
      std::printf("Mixer: master=%g%% softClip=%s inputs=%zu\n",
                  spec.mixer.masterPercent, spec.mixer.softClip ? "true" : "false", spec.mixer.inputs.size());
    }
    if (spec.hasTransport) {
      std::printf("Transport: bpm=%.2f bars=%u res=%u swing=%.1f%% patterns=%zu ramps=%zu\n",
                  spec.transport.bpm, spec.transport.lengthBars, spec.transport.resolution,
                  spec.transport.swingPercent, spec.transport.patterns.size(), spec.transport.tempoRamps.size());
    }
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "Failed to load graph JSON: %s\n", e.what());
    return 1;
  }
}

static int structuralCheckJson(const std::string& path) {
  int errors = 0;
  try {
    nlohmann::json j;
    {
      std::ifstream f(path);
      if (!f.good()) { std::fprintf(stderr, "Cannot open %s\n", path.c_str()); return 1; }
      f >> j;
    }
    if (!j.is_object()) { std::fprintf(stderr, "%s: top-level must be object\n", path.c_str()); return 2; }
    if (!j.contains("version") || !j["version"].is_number_integer()) {
      std::fprintf(stderr, "Missing/integer 'version'\n"); errors++;
    }
    if (!j.contains("nodes") || !j["nodes"].is_array()) {
      std::fprintf(stderr, "Missing/array 'nodes'\n"); errors++;
    } else {
      for (const auto& n : j["nodes"]) {
        if (!n.is_object()) { std::fprintf(stderr, "Node entry is not object\n"); errors++; continue; }
        if (!n.contains("id") || !n["id"].is_string()) { std::fprintf(stderr, "Node missing string 'id'\n"); errors++; }
        if (!n.contains("type") || !n["type"].is_string()) { std::fprintf(stderr, "Node missing string 'type'\n"); errors++; }
      }
    }
    if (j.contains("connections")) {
      const auto& arr = j["connections"];
      if (!arr.is_array()) { std::fprintf(stderr, "'connections' must be array\n"); errors++; }
      else {
        for (const auto& c : arr) {
          if (!c.is_object()) { std::fprintf(stderr, "Connection entry is not object\n"); errors++; continue; }
          if (!c.contains("from") || !c["from"].is_string()) { std::fprintf(stderr, "Connection missing string 'from'\n"); errors++; }
          if (!c.contains("to") || !c["to"].is_string()) { std::fprintf(stderr, "Connection missing string 'to'\n"); errors++; }
          if (c.contains("gainPercent") && !c["gainPercent"].is_number()) { std::fprintf(stderr, "Connection gainPercent must be number\n"); errors++; }
          if (c.contains("dryPercent") && !c["dryPercent"].is_number()) { std::fprintf(stderr, "Connection dryPercent must be number\n"); errors++; }
          if (c.contains("fromPort") && !c["fromPort"].is_number_integer()) { std::fprintf(stderr, "Connection fromPort must be integer\n"); errors++; }
          if (c.contains("toPort") && !c["toPort"].is_number_integer()) { std::fprintf(stderr, "Connection toPort must be integer\n"); errors++; }
        }
      }
    }
    return (errors == 0) ? 0 : 2;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "Schema structural check failed: %s\n", e.what());
    return 1;
  }
}

// removed legacy schemaEnforceJson (replaced by validateJsonWithDraft2020)

static int validateGraphJson(const std::string& path) {
  try {
    // Structural sanity (schema-aligned, lightweight)
    if (int s = structuralCheckJson(path); s == 1) return 1; else if (s == 2) { /* continue with detailed checks but mark */ }
    // Schema enforcement (best-effort using bundled schema)
    {
      const std::string schemaPath = std::string("docs/schema.graph.v1.json");
      std::string diag;
      int s2 = validateJsonWithDraft2020(path, schemaPath, diag);
      if (s2 == 1) return 1; // fatal
      if (s2 == 2) std::fprintf(stderr, "Schema: %s\n", diag.c_str());
      if (s2 == 3) std::fprintf(stderr, "Schema: %s\n", diag.c_str());
    }
    GraphSpec spec = loadGraphSpecFromJsonFile(path);
    int errors = 0;
    std::vector<std::string> nodeIds;
    nodeIds.reserve(spec.nodes.size());
    for (const auto& n : spec.nodes) nodeIds.push_back(n.id);
    auto hasNode = [&](const std::string& id) {
      return std::find(nodeIds.begin(), nodeIds.end(), id) != nodeIds.end();
    };
    // Check unique node ids
    {
      auto tmp = nodeIds; std::sort(tmp.begin(), tmp.end());
      for (size_t i = 1; i < tmp.size(); ++i) {
        if (tmp[i] == tmp[i-1]) { std::fprintf(stderr, "Duplicate node id: %s\n", tmp[i].c_str()); errors++; }
      }
    }
    // Known node types + minimal transport check
    for (const auto& n : spec.nodes) {
      if (!(n.type == "kick" || n.type == "clap" || n.type == "transport" || n.type == "delay" || n.type == "meter" || n.type == "compressor" || n.type == "reverb")) {
        std::fprintf(stderr, "Unknown node type '%s' (id=%s)\n", n.type.c_str(), n.id.c_str());
      }
      if (n.type == "transport") {
        try {
          nlohmann::json pj = nlohmann::json::parse(n.paramsJson);
          if (pj.contains("pattern")) {
            const auto& p = pj.at("pattern");
            const std::string target = p.value("nodeId", "");
            const std::string steps = p.value("steps", "");
            if (target.empty() || !hasNode(target)) { std::fprintf(stderr, "Transport node '%s' references unknown node '%s'\n", n.id.c_str(), target.c_str()); errors++; }
            if (steps.empty()) { std::fprintf(stderr, "Transport node '%s' has empty steps pattern\n", n.id.c_str()); errors++; }
          }
        } catch (...) {
          std::fprintf(stderr, "Transport node '%s' params parse failed\n", n.id.c_str()); errors++;
        }
      }
    }
    // Mixer inputs exist
    if (spec.hasMixer) {
      // dup check
      {
        std::vector<std::string> ids;
        ids.reserve(spec.mixer.inputs.size());
        for (const auto& mi : spec.mixer.inputs) ids.push_back(mi.id);
        auto tmp = ids; std::sort(tmp.begin(), tmp.end());
        for (size_t i = 1; i < tmp.size(); ++i) if (tmp[i] == tmp[i-1]) {
          std::fprintf(stderr, "Duplicate mixer input '%s'\n", tmp[i].c_str()); errors++;
        }
      }
      for (const auto& mi : spec.mixer.inputs) {
        if (!hasNode(mi.id)) { std::fprintf(stderr, "Mixer references unknown node '%s'\n", mi.id.c_str()); errors++; }
      }
    }
    // Commands
    for (const auto& c : spec.commands) {
      if (!hasNode(c.nodeId)) { std::fprintf(stderr, "Command references unknown node '%s'\n", c.nodeId.c_str()); errors++; continue; }
      if (c.type == "SetParam" || c.type == "SetParamRamp") {
        uint16_t pid = c.paramId;
        if (pid == 0 && !c.paramName.empty()) {
          // resolve by name
          std::string nodeType;
          for (const auto& n : spec.nodes) if (n.id == c.nodeId) { nodeType = n.type; break; }
          if (nodeType == "kick") pid = resolveParamIdByName(kKickParamMap, c.paramName);
          else if (nodeType == "clap") pid = resolveParamIdByName(kClapParamMap, c.paramName);
          else if (nodeType == "tb303_ext") pid = resolveParamIdByName(kTb303ParamMap, c.paramName);
        }
        if (pid == 0) { std::fprintf(stderr, "Command missing/unknown param (node=%s)\n", c.nodeId.c_str()); errors++; }
      }
    }
    // Transport patterns and locks
    if (spec.hasTransport) {
      const uint32_t stepsPerBar = spec.transport.resolution ? spec.transport.resolution : 16u;
      // Build nodeId->type for param name validation
      std::unordered_map<std::string, std::string> nodeIdToType;
      for (const auto& ns : spec.nodes) nodeIdToType.emplace(ns.id, ns.type);
      auto mapParam = [](const std::string& type, const std::string& name) -> uint16_t {
        if (type == std::string("kick")) return resolveParamIdByName(kKickParamMap, name);
        if (type == std::string("clap")) return resolveParamIdByName(kClapParamMap, name);
        if (type == std::string("tb303_ext")) return resolveParamIdByName(kTb303ParamMap, name);
        return 0;
      };
      for (const auto& p : spec.transport.patterns) {
        if (!hasNode(p.nodeId)) { std::fprintf(stderr, "Pattern references unknown node '%s'\n", p.nodeId.c_str()); errors++; }
        if (p.steps.empty()) { std::fprintf(stderr, "Pattern for node '%s' has empty steps\n", p.nodeId.c_str()); errors++; }
        if (!p.steps.empty() && p.steps.size() != stepsPerBar) {
          std::fprintf(stderr, "Pattern for node '%s' has %zu steps but resolution is %u\n", p.nodeId.c_str(), p.steps.size(), stepsPerBar);
        }
        for (const auto& L : p.locks) {
          if (L.step >= stepsPerBar) { std::fprintf(stderr, "Lock step %u out of range for node '%s' (res=%u)\n", L.step, p.nodeId.c_str(), stepsPerBar); errors++; }
          if (L.paramId == 0 && L.paramName.empty()) { std::fprintf(stderr, "Lock missing param for node '%s'\n", p.nodeId.c_str()); errors++; }
          if (L.paramId == 0 && !L.paramName.empty()) {
            uint16_t pid = 0;
            auto it = nodeIdToType.find(p.nodeId);
            if (it != nodeIdToType.end()) pid = mapParam(it->second, L.paramName);
            if (pid == 0) { std::fprintf(stderr, "Lock has unknown param '%s' for node '%s'\n", L.paramName.c_str(), p.nodeId.c_str()); errors++; }
          }
        }
      }
    }

    // Connections validation and cycle check (Kahn)
    if (!spec.connections.empty()) {
      // endpoints exist
      {
        std::unordered_set<std::string> edgeSet;
        for (const auto& c : spec.connections) {
          const std::string key = c.from + std::string("->") + c.to;
          if (!edgeSet.insert(key).second) {
            std::fprintf(stderr, "Duplicate connection %s\n", key.c_str()); errors++;
          }
        }
      }
      for (const auto& c : spec.connections) {
        if (!hasNode(c.from)) { std::fprintf(stderr, "Connection 'from' unknown node '%s'\n", c.from.c_str()); errors++; }
        if (!hasNode(c.to)) { std::fprintf(stderr, "Connection 'to' unknown node '%s'\n", c.to.c_str()); errors++; }
        if (c.from == c.to) { std::fprintf(stderr, "Connection self-edge not allowed: %s->%s\n", c.from.c_str(), c.to.c_str()); errors++; }
        if (c.gainPercent < 0.0f || c.gainPercent > 200.0f) {
          std::fprintf(stderr, "Connection %s->%s gainPercent out of range: %g\n", c.from.c_str(), c.to.c_str(), c.gainPercent); errors++;
        }
      }
      // cycle detection
      std::unordered_map<std::string,int> indeg;
      for (const auto& n : spec.nodes) indeg[n.id] = 0;
      for (const auto& e : spec.connections) if (indeg.find(e.to)!=indeg.end()) indeg[e.to]++;
      std::unordered_multimap<std::string,std::string> adj;
      for (const auto& e : spec.connections) adj.emplace(e.from, e.to);
      std::vector<std::string> q; q.reserve(indeg.size());
      for (const auto& kv : indeg) if (kv.second==0) q.push_back(kv.first);
      size_t visited = 0;
      for (size_t i = 0; i < q.size(); ++i) {
        const auto u = q[i]; visited++;
        auto range = adj.equal_range(u);
        for (auto it = range.first; it != range.second; ++it) {
          auto& v = it->second; if (--indeg[v]==0) q.push_back(v);
        }
      }
      if (visited != indeg.size()) {
        std::fprintf(stderr, "Connections contain a cycle (visited %zu of %zu)\n", visited, indeg.size());
        errors++;
      }
      // dryPercent range
      for (const auto& c : spec.connections) {
        if (c.dryPercent < 0.0f || c.dryPercent > 200.0f) {
          std::fprintf(stderr, "Connection %s->%s dryPercent out of range: %g\n", c.from.c_str(), c.to.c_str(), c.dryPercent); errors++;
        }
      }
      // mixer input validation and dry/wet ergonomics
      if (spec.hasMixer) {
        // duplicate mixer inputs
        {
          std::unordered_map<std::string, int> mixCount;
          for (const auto& mi : spec.mixer.inputs) mixCount[mi.id]++;
          for (const auto& kv : mixCount) {
            if (kv.second > 1) {
              std::fprintf(stderr, "Warning: duplicate mixer input id '%s' appears %d times\n", kv.first.c_str(), kv.second);
            }
          }
        }
        // warn if dry tap and also mixed directly (double-count risk)
        {
          std::unordered_set<std::string> mixed;
          for (const auto& mi : spec.mixer.inputs) mixed.insert(mi.id);
          for (const auto& c : spec.connections) {
            if (c.dryPercent > 0.0f && mixed.count(c.from)) {
              std::fprintf(stderr, "Warning: %s is in mixer inputs and has dryPercent>0 on edge %s->%s (double-count).\n",
                           c.from.c_str(), c.from.c_str(), c.to.c_str());
            }
          }
        }
      }
      // fromPort/toPort validation against declared ports (if provided)
      {
        std::unordered_map<std::string, std::unordered_set<uint32_t>> inPorts;
        std::unordered_map<std::string, std::unordered_set<uint32_t>> outPorts;
        std::unordered_map<std::string, std::unordered_map<uint32_t, std::string>> inTypes;
        std::unordered_map<std::string, std::unordered_map<uint32_t, std::string>> outTypes;
        for (const auto& n : spec.nodes) {
          if (n.ports.has) {
            for (const auto& ip : n.ports.inputs) { inPorts[n.id].insert(ip.index); inTypes[n.id][ip.index] = ip.type; }
            for (const auto& op : n.ports.outputs) { outPorts[n.id].insert(op.index); outTypes[n.id][op.index] = op.type; }
          }
        }
        // Warn if audio port channel count mismatches graph channels (simple MVP policy)
        for (const auto& n : spec.nodes) {
          if (!n.ports.has) continue;
          for (const auto& ip : n.ports.inputs) {
            if (ip.type == std::string("audio") && ip.channels != 0u && ip.channels != spec.channels) {
              std::fprintf(stderr, "Warning: node %s input port %u channels=%u != graph channels=%u (adapter not yet implemented)\n",
                           n.id.c_str(), ip.index, ip.channels, spec.channels);
            }
          }
          for (const auto& op : n.ports.outputs) {
            if (op.type == std::string("audio") && op.channels != 0u && op.channels != spec.channels) {
              std::fprintf(stderr, "Warning: node %s output port %u channels=%u != graph channels=%u (adapter not yet implemented)\n",
                           n.id.c_str(), op.index, op.channels, spec.channels);
            }
          }
        }
        for (const auto& c : spec.connections) {
          if (outPorts.count(c.from) && !outPorts[c.from].count(c.fromPort)) {
            std::fprintf(stderr, "Connection %s->%s references unknown fromPort %u\n", c.from.c_str(), c.to.c_str(), c.fromPort); errors++;
          }
          if (inPorts.count(c.to) && !inPorts[c.to].count(c.toPort)) {
            std::fprintf(stderr, "Connection %s->%s references unknown toPort %u\n", c.from.c_str(), c.to.c_str(), c.toPort); errors++; }
          // Type compatibility (audio->audio only for now)
          if (outTypes.count(c.from) && outTypes[c.from].count(c.fromPort) && inTypes.count(c.to) && inTypes[c.to].count(c.toPort)) {
            const std::string& ft = outTypes[c.from][c.fromPort];
            const std::string& tt = inTypes[c.to][c.toPort];
            if (!(ft == std::string("audio") && tt == std::string("audio"))) {
              std::fprintf(stderr, "Connection %s(%s)->%s(%s) port types incompatible\n", c.from.c_str(), ft.c_str(), c.to.c_str(), tt.c_str()); errors++;
            }
          }
        }
      }
      // fromPort/toPort supported (aggregated) – no warnings
    }

    // Type-specific param sanity checks
    for (const auto& n : spec.nodes) {
      if (n.type == std::string("delay")) {
        try {
          nlohmann::json pj = nlohmann::json::parse(n.paramsJson);
          const double mix = pj.value("mix", 1.0);
          const double fb = pj.value("feedback", 0.0);
          if (mix < 0.0 || mix > 1.0) {
            std::fprintf(stderr, "Delay '%s' mix out of range [0..1]: %g\n", n.id.c_str(), mix); errors++;
          }
          if (fb < 0.0 || fb >= 0.98) {
            std::fprintf(stderr, "Delay '%s' feedback suspicious (>=0.98 may blow up): %g\n", n.id.c_str(), fb);
          }
        } catch (...) {
          std::fprintf(stderr, "Delay '%s' params parse failed\n", n.id.c_str()); errors++;
        }
      } else if (n.type == std::string("meter")) {
        try {
          nlohmann::json pj = nlohmann::json::parse(n.paramsJson);
          const std::string target = pj.value("target", std::string());
          if (target.empty() || !hasNode(target)) {
            std::fprintf(stderr, "Meter '%s' target unknown node '%s'\n", n.id.c_str(), target.c_str()); errors++;
          }
        } catch (...) {
          std::fprintf(stderr, "Meter '%s' params parse failed\n", n.id.c_str()); errors++;
        }
      }
    }
    if (errors == 0) {
      std::printf("%s: OK\n", path.c_str());
      return 0;
    }
    return 2;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "Failed to load graph JSON: %s\n", e.what());
    return 1;
  }
}

// Diagnostics: dump command list with bar/step using provided bpm/resolution/sampleRate
static void dumpCommands(const std::vector<GraphSpec::CommandSpec>& cmds,
                         uint32_t sr,
                         double bpm,
                         uint32_t resolution,
                         const char* tag) {
  const double secPerBeat = (bpm > 0.0) ? (60.0 / bpm) : (60.0 / 120.0);
  const double secPerBar = 4.0 * secPerBeat;
  const uint64_t framesPerBar = static_cast<uint64_t>(secPerBar * static_cast<double>(sr) + 0.5);
  for (const auto& c : cmds) {
    const uint64_t st = c.sampleTime;
    const uint64_t barIdx = (framesPerBar > 0) ? (st / framesPerBar) : 0ull;
    const uint64_t withinBar = (framesPerBar > 0) ? (st % framesPerBar) : st;
    const uint64_t stepIdx = (framesPerBar > 0 && resolution > 0)
      ? ((withinBar * static_cast<uint64_t>(resolution) + framesPerBar / 2) / framesPerBar)
      : 0ull;
    const char* typeStr = (c.type == std::string("Trigger")) ? "TRIG"
                          : (c.type == std::string("SetParam")) ? "SET"
                          : (c.type == std::string("SetParamRamp")) ? "RAMP" : "?";
    std::fprintf(stderr, "%s: t=%llu bar=%llu step=%llu node=%s type=%s pid=%u val=%.3f rampMs=%.1f param=%s\n",
                 tag,
                 static_cast<unsigned long long>(st),
                 static_cast<unsigned long long>(barIdx + 1ull),
                 static_cast<unsigned long long>((stepIdx % (resolution ? resolution : 16u)) + 1ull),
                 c.nodeId.c_str(), typeStr, c.paramId, c.value, c.rampMs,
                 c.paramName.empty() ? "" : c.paramName.c_str());
  }
}

int main(int argc, char** argv) {
  const auto entry = mam::app::selectCliEntry(argc, argv);
  if (entry == mam::app::CliEntry::BuildInfo) {
    std::puts(mamBuildInfoJson());
    return 0;
  }
  if (entry == mam::app::CliEntry::PythonTools)
    return runPythonTools(argc - 2, argv + 2);
  if (entry == mam::app::CliEntry::Stems)
    return runStemRenderer(argc - 2, argv + 2);
  mam::app::CliOptions options;
  std::fprintf(stderr, "mam -- version %s starting up (revision %s)\n", mamVersion(), mamGitRevision());
  if (auto result = mam::app::parseCliOptions(argc, argv, options)) return *result;
  gOfflineProgressMs = options.offlineProgressMs;
  gOfflineProgressEnabled = options.offlineProgressEnabled;
  gOfflineSummaryEnabled = options.offlineSummaryEnabled;
  auto& params = options.params;
  auto& wavPath = options.wavPath;
  auto& outFormat = options.outFormat;
  auto& outDepth = options.outDepth;
  auto& graphPath = options.graphPath;
  auto& rackPath = options.rackPath;
  auto& sessionPath = options.sessionPath;
  auto& validatePath = options.validatePath;
  auto& listNodesPath = options.listNodesPath;
  auto& listParamsType = options.listParamsType;
  auto& listNodeTypes = options.listNodeTypes;
  auto& offlineSr = options.offlineSr;
  auto& pcm16 = options.pcm16;
  auto& quitAfterSec = options.quitAfterSec;
  auto& offlineThreads = options.offlineThreads;
  auto& autoWavName = options.autoWavName;
  auto& overrideDurationSec = options.overrideDurationSec;
  auto& overrideBars = options.overrideBars;
  auto& overrideLoopCount = options.overrideLoopCount;
  auto& overrideStartBar = options.overrideStartBar;
  auto& overrideEndBar = options.overrideEndBar;
  auto& loopMinutes = options.loopMinutes;
  auto& loopSeconds = options.loopSeconds;
  auto& tailMs = options.tailMs;
  auto& doNormalize = options.doNormalize;
  auto& peakTargetDb = options.peakTargetDb;
  auto& printTopo = options.printTopo;
  auto& printPorts = options.printPorts;
  auto& printMeters = options.printMeters;
  auto& tailOverridden = options.tailOverridden;
  auto& verbose = options.verbose;
  auto& topoVerbose = options.topoVerbose;
  auto& offlineScheduler = options.offlineScheduler;
  auto& offlineBlock = options.offlineBlock;
  auto& topoThreads = options.topoThreads;
  auto& topoMinWidth = options.topoMinWidth;
  auto& topoMinSegFrames = options.topoMinSegFrames;
  auto& topoTracePath = options.topoTracePath;
  auto& randomSeedOverride = options.randomSeedOverride;
  auto& metersPerNode = options.metersPerNode;
  auto& exportMermaidSessionPath = options.exportMermaidSessionPath;
  auto& exportMermaidGraphPath = options.exportMermaidGraphPath;
  auto& traceJsonPath = options.traceJsonPath;
  auto& printSha1 = options.printSha1;
  auto& cpuStats = options.cpuStats;
  auto& cpuStatsPerNode = options.cpuStatsPerNode;
  auto& rtDebugFeed = options.rtDebugFeed;
  auto& printTriggers = options.printTriggers;
  auto& dumpEvents = options.dumpEvents;
  auto& schemaStrict = options.schemaStrict;
  auto& printLatency = options.printLatency;

  if (!exportMermaidSessionPath.empty()) {
    try {
      SessionSpec sess = loadSessionSpecFromJsonFile(exportMermaidSessionPath);
      const std::string mmd = generateMermaidForSession(sess);
      std::fwrite(mmd.data(), 1, mmd.size(), stdout);
      return 0;
    } catch (const std::exception& e) {
      std::fprintf(stderr, "Diagram export failed: %s\n", e.what());
      return 1;
    }
  }
  if (!exportMermaidGraphPath.empty()) {
    try {
      GraphSpec spec = loadGraphSpecFromJsonFile(exportMermaidGraphPath);
      const std::string mmd = generateMermaidForGraph(spec);
      std::fwrite(mmd.data(), 1, mmd.size(), stdout);
      return 0;
    } catch (const std::exception& e) {
      std::fprintf(stderr, "Diagram export failed: %s\n", e.what());
      return 1;
    }
  }

  params.loop = params.bpm > 0.0f;
  if (params.gain < 0.0f) params.gain = 0.0f;
  if (params.gain > 1.5f) params.gain = 1.5f;
  if (params.click < 0.0f) params.click = 0.0f;
  if (params.click > 1.0f) params.click = 1.0f;

  // If --wav was provided without a filename, set a placeholder so we take the offline path;
  // we'll generate the final name (with frames) right before writing.
  if (autoWavName && wavPath.empty()) wavPath = "__AUTO__";

  if (schemaStrict) {
    std::string diagnostic;
    if (validateJsonWithDraft2020(graphPath, "docs/schema.graph.v1.json", diagnostic) != 0) {
      std::fprintf(stderr, "Schema validation failed: %s\n", diagnostic.c_str());
      return 1;
    }
  }

  // Utilities
  if (rackPath.size() && graphPath.empty()) graphPath = rackPath;
  if ((printTopo || printPorts) && !graphPath.empty()) {
    try {
      GraphSpec spec = loadGraphSpecFromJsonFile(graphPath);
      warnSidechainConnectivity(spec);
      if (printTopo) { printTopoOrderFromSpec(spec); printConnectionsSummary(spec); }
      if (printPorts) { printPortsSummary(spec); }
    } catch (...) {}
    // If only inspecting topology, exit early to avoid entering realtime
    if (wavPath.empty() && validatePath.empty() && listNodesPath.empty() && listParamsType.empty()) {
      return 0;
    }
  }
  if (!validatePath.empty()) return validateGraphJson(validatePath);
  if (!listNodesPath.empty()) return listNodesGraphJson(listNodesPath);
  if (listNodeTypes) {
    try {
      nlohmann::json schema;
      std::ifstream fs("docs/schema.graph.v1.json");
      if (fs.good()) {
        fs >> schema;
        const auto& enumArr = schema.at("properties").at("nodes").at("items").at("properties").at("type").at("enum");
        std::printf("Supported node types (%zu):\n", enumArr.size());
        for (const auto& e : enumArr) std::printf("- %s\n", e.get<std::string>().c_str());
      } else {
        std::printf("Supported node types: kick, clap, transport, delay, meter, compressor, reverb\n");
      }
    } catch (...) {
      std::printf("Supported node types: kick, clap, transport, delay, meter, compressor, reverb\n");
    }
    return 0;
  }
  if (!listParamsType.empty()) {
    if (listParamsType == std::string("kick")) {
      std::printf("kick params:\n");
      for (size_t idx = 0; idx < kKickParamMap.count; ++idx) {
        const auto& d = kKickParamMap.defs[idx];
        std::printf("%u %s [%g..%g] def=%g %s\n", d.id, d.name, d.minValue, d.maxValue, d.defaultValue, d.smoothing);
      }
      return 0;
    } else if (listParamsType == std::string("clap")) {
      std::printf("clap params:\n");
      for (size_t idx = 0; idx < kClapParamMap.count; ++idx) {
        const auto& d = kClapParamMap.defs[idx];
        std::printf("%u %s [%g..%g] def=%g %s\n", d.id, d.name, d.minValue, d.maxValue, d.defaultValue, d.smoothing);
      }
      return 0;
    } else {
      std::fprintf(stderr, "Unknown node type for --list-params: %s\n", listParamsType.c_str());
      return 1;
    }
  }

  std::signal(SIGINT, onSigInt);

  if (!sessionPath.empty() && wavPath.empty())
    return mam::app::runRealtimeSession(options, gRunning, isStdinReady);

  // Offline render path to WAV if requested
  if (!wavPath.empty()) {
    const uint32_t channels = 2;
    const uint32_t sr = static_cast<uint32_t>(offlineSr + 0.5);
    uint64_t totalFrames = 0;

    // Session offline path
    if (!sessionPath.empty()) {
      try {
        SessionSpec sess = loadSessionSpecFromJsonFile(sessionPath);
        if (offlineSr > 0.0) sess.sampleRate = sr;
        SessionRuntime runtime; runtime.loadFromSpec(sess);

        // Handle loop-aware duration planning
        uint32_t maxLoops = 1;
        if (sess.loop && sess.durationSec > 0.0) {
          // For looping sessions, calculate frames for multiple loops
          uint64_t singleLoopFrames = runtime.planTotalFrames(tailMs, false, 1);
          if (singleLoopFrames > 0) {
            double singleLoopSec = static_cast<double>(singleLoopFrames) / sr;
            maxLoops = static_cast<uint32_t>(std::ceil(sess.durationSec / singleLoopSec));
            totalFrames = runtime.planTotalFrames(tailMs, true, maxLoops);
            std::fprintf(stderr, "[offline-session] looping session: %u loops (%.3fs each), total duration=%.3f sec\n",
              maxLoops, singleLoopSec, static_cast<double>(totalFrames) / sr);
          } else {
            totalFrames = runtime.planTotalFrames(tailMs);
          }
        } else {
          totalFrames = runtime.planTotalFrames(tailMs);
        }

        if (overrideDurationSec >= 0.0) totalFrames = static_cast<uint64_t>(overrideDurationSec * static_cast<double>(sr) + 0.5);
        // Enable per-rack meters if requested
        std::vector<SessionRuntime::RackStats> rstats;
        runtime.setPerRackMeters(printMeters);
        runtime.setPerRackCpu(cpuStats || cpuStatsPerNode);

        std::vector<float> interleaved;
        if (sess.loop && sess.durationSec > 0.0 && maxLoops > 1) {
          // Use loop-aware rendering for looping sessions
          std::fprintf(stderr, "[offline-session] rendering %u loops for session\n", maxLoops);
          interleaved = runtime.renderOfflineWithLoop(totalFrames, maxLoops, printMeters ? &rstats : nullptr);
        } else {
          // Standard rendering for non-looping sessions
          interleaved = runtime.renderOffline(totalFrames, printMeters ? &rstats : nullptr);
        }
        AudioFileSpec spec; spec.format = outFormat; spec.bitDepth = pcm16 ? BitDepth::Pcm16 : outDepth; spec.sampleRate = sr; spec.channels = channels;
        writeWithExtAudioFile(wavPath, spec, interleaved);
        const double seconds = static_cast<double>(totalFrames) / static_cast<double>(sr);
        std::fprintf(stderr, "Exported session to %s (frames=%llu, %.3fs)\n", wavPath.c_str(), (unsigned long long)interleaved.size()/channels, seconds);
        if (printMeters && !rstats.empty()) {
          for (const auto& st : rstats) {
            std::fprintf(stderr, "  Rack %s: peak=%.2f dBFS rms=%.2f dBFS\n", st.id.c_str(), st.peakDb, st.rmsDb);
          }
        }
        return 0;
      } catch (const std::exception& e) {
        std::fprintf(stderr, "Session render failed: %s\n", e.what());
        return 1;
      }
    }

    Graph graph;
    if (!graphPath.empty()) {
      try {
        if (schemaStrict) {
          std::string diag;
          const std::string schemaPath = std::string("docs/schema.graph.v1.json");
          int vs = validateJsonWithDraft2020(graphPath, schemaPath, diag);
          if (vs != 0) { std::fprintf(stderr, "Schema validation failed: %s\n", diag.c_str()); return 1; }
        }
        GraphSpec spec = loadGraphSpecFromJsonFile(graphPath);
        warnSidechainConnectivity(spec);
        warnDrySuppression(spec);
        if (randomSeedOverride != 0) setGlobalSeed(randomSeedOverride);
        else if (spec.randomSeed != 0) setGlobalSeed(spec.randomSeed);
        for (const auto& ns : spec.nodes) {
          auto node = createNodeFromSpec(ns);
          if (node) graph.addNode(ns.id, std::move(node));
        }
        if (spec.hasMixer) {
          std::vector<MixerChannel> chans;
          for (const auto& inp : spec.mixer.inputs) {
            MixerChannel mc; mc.id = inp.id; mc.gain = inp.gainPercent * (1.0f/100.0f);
            chans.push_back(mc);
          }
          const float master = spec.mixer.masterPercent * (1.0f/100.0f);
          graph.setMixer(std::make_unique<MixerNode>(std::move(chans), master, spec.mixer.softClip));
        }
      if (!spec.connections.empty()) {
          graph.setConnections(spec.connections);
        }
      // Provide port descriptors to graph (for channel adapters)
      graph.setPortDescriptors(spec.nodes);
      if (printTopo) printTopoOrderFromSpec(spec);
      if (metersPerNode) graph.enableStats(true);
      if (cpuStats || cpuStatsPerNode) graph.enableCpuStats(true);
      if (!traceJsonPath.empty()) graph.enableTrace(traceJsonPath.c_str());
      if (cpuStats || cpuStatsPerNode) graph.enableCpuStats(true);
      } catch (const std::exception& e) {
        std::fprintf(stderr, "Failed to load graph JSON: %s\n", e.what());
        return 1;
      }
    } else {
      KickParams p = params;
      p.loop = false;
      graph.addNode("kick_default", std::make_unique<KickNode>(p));
    }
    std::vector<float> interleaved;
    double prerollMsForSummary = 0.0; // computed when preroll is applied in offline export
    if (!graphPath.empty()) {
      try {
        GraphSpec spec2 = loadGraphSpecFromJsonFile(graphPath);
        if (randomSeedOverride != 0) setGlobalSeed(randomSeedOverride);
        else if (spec2.randomSeed != 0) setGlobalSeed(spec2.randomSeed);
        // Synthesize commands from transport, if present
        std::vector<GraphSpec::CommandSpec> cmds = spec2.commands;
        if (spec2.hasTransport) {
          // Generate transport commands covering requested bars/loops
          GraphSpec::Transport tgen = spec2.transport;
          const uint32_t baseBars = spec2.transport.lengthBars ? spec2.transport.lengthBars : 1u;
          uint32_t useBars = overrideBars > 0 ? overrideBars : baseBars;
          uint32_t loops = overrideLoopCount > 0 ? overrideLoopCount : 1u;
          if ((loopMinutes > 0.0 || loopSeconds > 0.0) && useBars > 0) {
            const double targetSec = (loopMinutes > 0.0 ? loopMinutes * 60.0 : loopSeconds);
            if (targetSec > 0.0) {
              // compute seconds per bar with ramps by averaging first bar
              const double bpm = spec2.transport.bpm > 0.0 ? spec2.transport.bpm : 120.0;
              const double secPerBar = 4.0 * (60.0 / bpm);
              const uint32_t perLoopBars = useBars;
              const double perLoopSec = secPerBar * static_cast<double>(perLoopBars);
              loops = static_cast<uint32_t>(std::ceil(targetSec / std::max(0.001, perLoopSec)));
              if (loops == 0) loops = 1;
            }
          }
          tgen.lengthBars = useBars * loops;
          auto gen = generateCommandsFromTransport(tgen, sr);
          // Optional slicing to a bar range [startBar, endBar)
          if (overrideStartBar > 0 || overrideEndBar > 0) {
            const uint32_t startBar = overrideStartBar;
            const uint32_t endBar = (overrideEndBar > 0) ? overrideEndBar : tgen.lengthBars;
            const double secPerBar = 4.0 * (60.0 / (tgen.bpm > 0.0 ? tgen.bpm : 120.0));
            const uint64_t framesPerBar0 = static_cast<uint64_t>(secPerBar * static_cast<double>(sr) + 0.5);
            const uint64_t startFrames = framesPerBar0 * static_cast<uint64_t>(startBar);
            const uint64_t endFrames = framesPerBar0 * static_cast<uint64_t>(std::max(endBar, startBar));
            gen.erase(std::remove_if(gen.begin(), gen.end(), [&](const auto& c){ return c.sampleTime < startFrames || c.sampleTime >= endFrames; }), gen.end());
            for (auto& c : gen) c.sampleTime -= startFrames;
          }
          cmds.insert(cmds.end(), gen.begin(), gen.end());
        }
        // Resolve named params to IDs based on node type
        {
          std::unordered_map<std::string, std::string> nodeIdToType;
          for (const auto& ns : spec2.nodes) nodeIdToType.emplace(ns.id, ns.type);
          auto mapParam = [](const std::string& type, const std::string& name) -> uint16_t {
            if (type == std::string("kick")) return resolveParamIdByName(kKickParamMap, name);
            if (type == std::string("clap")) return resolveParamIdByName(kClapParamMap, name);
            if (type == std::string("tb303_ext")) return resolveParamIdByName(kTb303ParamMap, name);
            return 0;
          };
          for (auto& c : cmds) {
            if (c.paramId == 0 && !c.paramName.empty()) {
              auto it = nodeIdToType.find(c.nodeId);
              const std::string nodeType = (it != nodeIdToType.end()) ? it->second : std::string();
              c.paramId = mapParam(nodeType, c.paramName);
            }
          }
        }
        // Optional event dump (offline): print synthesized commands with timing
        if (dumpEvents) {
          const double bpm = spec2.hasTransport ? spec2.transport.bpm : 120.0;
          const uint32_t res = spec2.hasTransport ? spec2.transport.resolution : 16u;
          dumpCommands(cmds, sr, bpm, res, "CMD");
        }
        // Determine totalFrames (auto unless overridden)
        if (overrideDurationSec >= 0.0) {
          totalFrames = static_cast<uint64_t>(overrideDurationSec * static_cast<double>(sr) + 0.5);
        } else if (spec2.hasTransport) {
          auto bpmAtBar = [&](uint32_t barIndex) -> double {
            double bpm = spec2.transport.bpm;
            for (const auto& p : spec2.transport.tempoRamps) { if (p.bar <= barIndex) bpm = p.bpm; }
            return bpm;
          };
          auto framesPerBarAt = [&](uint32_t barIndex) -> uint64_t {
            const double secPerBeat = 60.0 / bpmAtBar(barIndex);
            const double secPerBar = 4.0 * secPerBeat;
            return static_cast<uint64_t>(secPerBar * static_cast<double>(sr) + 0.5);
          };
          const uint32_t baseBars = spec2.transport.lengthBars ? spec2.transport.lengthBars : 1u;
          const uint32_t useBars = overrideBars > 0 ? overrideBars : baseBars;
          const uint32_t startBar = overrideStartBar;
          const uint32_t endBar = (overrideEndBar > 0) ? overrideEndBar : useBars;
          const uint32_t loops = overrideLoopCount > 0 ? overrideLoopCount : 1u;
          const uint64_t spanBars = static_cast<uint64_t>((endBar > startBar ? (endBar - startBar) : 0u));
          const uint64_t totalBars = spanBars * static_cast<uint64_t>(loops);
          totalFrames = 0;
          for (uint64_t b = 0; b < totalBars; ++b) totalFrames += framesPerBarAt(static_cast<uint32_t>(startBar + b));
        } else if (!cmds.empty()) {
          uint64_t last = 0; for (const auto& c : cmds) if (c.sampleTime > last) last = c.sampleTime;
          totalFrames = last;
        } else {
          totalFrames = static_cast<uint64_t>(2.0 * static_cast<double>(sr) + 0.5);
        }
    // Add preroll (graph latency) and tail
        // Suggest longer tail for long delays/reverb if not overridden
        double tailMsLocal = tailMs;
        if (!tailOverridden) {
          double maxDelayMs = 0.0; bool hasReverb = false;
          for (const auto& ns : spec2.nodes) {
            if (ns.type == std::string("delay")) {
              try { nlohmann::json pj = nlohmann::json::parse(ns.paramsJson); maxDelayMs = std::max(maxDelayMs, pj.value("delayMs", 0.0)); } catch (...) {}
            } else if (ns.type == std::string("reverb")) {
              hasReverb = true;
            }
          }
          double suggested = 250.0;
          if (maxDelayMs > 0.0) suggested = std::max(suggested, std::min(6000.0, maxDelayMs * 2.0));
          if (hasReverb) suggested = std::max(suggested, 1000.0);
          tailMsLocal = suggested;
        }
        const uint64_t preroll = computeGraphPrerollSamples(spec2, sr);
        prerollMsForSummary = 1000.0 * static_cast<double>(preroll) / static_cast<double>(sr);
        totalFrames += preroll + static_cast<uint64_t>((tailMsLocal / 1000.0) * static_cast<double>(sr) + 0.5);
        // Print planned duration info when looping or bars override is used
        if (overrideBars > 0 || overrideLoopCount > 0 || loopMinutes > 0.0 || loopSeconds > 0.0) {
          const double plannedSec = static_cast<double>(totalFrames) / static_cast<double>(sr);
          std::fprintf(stderr, "Planned duration: %s (%.3fs) including preroll %.3fs and tail %.3fs\n",
                       formatDuration(plannedSec).c_str(), plannedSec,
                       static_cast<double>(preroll) / static_cast<double>(sr), tailMsLocal / 1000.0);
        }
        if (offlineScheduler == std::string("topo")) {
          OfflineTopoScheduler sched(channels);
          sched.setDebug(topoVerbose || verbose);
          sched.setBlockSize(offlineBlock);
          if (topoThreads == 0) {
            // Default threads from HW if user didn't specify
            const uint32_t hw = std::max(1u, static_cast<uint32_t>(std::thread::hardware_concurrency()));
            sched.setParallelism(hw);
          } else {
            sched.setParallelism(topoThreads);
          }
          sched.setParallelHeuristics(topoMinWidth, topoMinSegFrames);
          if (cpuStats || cpuStatsPerNode) sched.enableCpuStats(true);
          if (!topoTracePath.empty()) sched.enableTrace(topoTracePath.c_str());
          std::vector<GraphSpec::Connection> conns = spec2.connections;
          sched.render(graph, conns, cmds, sr, channels, totalFrames, interleaved);
          if (cpuStats || cpuStatsPerNode) {
            const auto s = sched.getCpuSummary();
            std::fprintf(stderr, "[offline-topo] CPU block avg=%.3fms max=%.3fms (avg=%.1f%% max=%.1f%%) blocks=%llu overruns=%llu | barrier avg=%.3fms max=%.3fms (levels=%llu, parallel=%llu)\n",
                         s.avgMs, s.maxMs, s.avgPercent, s.maxPercent,
                         (unsigned long long)s.blocks, (unsigned long long)s.overruns,
                         s.barrierAvgMs, s.barrierMaxMs,
                         (unsigned long long)s.barrierCount, (unsigned long long)s.parallelLevels);
            if (cpuStatsPerNode) {
              auto v = sched.getPerNodeCpu();
              for (const auto& e : v) std::fprintf(stderr, "  node=%s avg=%.2fus max=%.2fus\n", e.id.c_str(), e.avgUs, e.maxUs);
            }
          }
          if (!topoTracePath.empty()) sched.flushTrace();
        } else {
          interleaved = renderGraphWithCommands(graph, cmds, sr, channels, totalFrames);
        }
      } catch (...) {
        if (totalFrames == 0) totalFrames = static_cast<uint64_t>(2.0 * static_cast<double>(sr) + 0.5);
        interleaved = (offlineThreads > 1) ? renderGraphInterleavedParallel(graph, sr, channels, totalFrames, offlineThreads)
                                           : renderGraphInterleaved(graph, sr, channels, totalFrames);
      }
    } else {
      // No graph file: use duration override or default + tail
      if (overrideDurationSec >= 0.0) totalFrames = static_cast<uint64_t>(overrideDurationSec * static_cast<double>(sr) + 0.5);
      else totalFrames = static_cast<uint64_t>(2.0 * static_cast<double>(sr) + 0.5);
      totalFrames += static_cast<uint64_t>((tailMs / 1000.0) * static_cast<double>(sr) + 0.5);
      if (offlineScheduler == std::string("topo")) {
        OfflineTopoScheduler sched(channels);
        sched.setDebug(topoVerbose || verbose);
        sched.setBlockSize(offlineBlock);
        if (topoThreads == 0) {
          const uint32_t hw = std::max(1u, static_cast<uint32_t>(std::thread::hardware_concurrency()));
          sched.setParallelism(hw);
        } else {
          sched.setParallelism(topoThreads);
        }
        sched.setParallelHeuristics(topoMinWidth, topoMinSegFrames);
        if (cpuStats || cpuStatsPerNode) sched.enableCpuStats(true);
        std::vector<GraphSpec::Connection> conns;
        std::vector<GraphSpec::CommandSpec> empty;
        sched.render(graph, conns, empty, sr, channels, totalFrames, interleaved);
        if (cpuStats || cpuStatsPerNode) {
          const auto s = sched.getCpuSummary();
          std::fprintf(stderr, "[offline-topo] CPU block avg=%.3fms max=%.3fms (avg=%.1f%% max=%.1f%%) blocks=%llu overruns=%llu | barrier avg=%.3fms max=%.3fms (levels=%llu, parallel=%llu)\n",
                       s.avgMs, s.maxMs, s.avgPercent, s.maxPercent,
                       (unsigned long long)s.blocks, (unsigned long long)s.overruns,
                       s.barrierAvgMs, s.barrierMaxMs,
                       (unsigned long long)s.barrierCount, (unsigned long long)s.parallelLevels);
          if (cpuStatsPerNode) {
            auto v = sched.getPerNodeCpu();
            for (const auto& e : v) std::fprintf(stderr, "  node=%s avg=%.2fus max=%.2fus\n", e.id.c_str(), e.avgUs, e.maxUs);
          }
        }
      } else {
        interleaved = (offlineThreads > 1) ? renderGraphInterleavedParallel(graph, sr, channels, totalFrames, offlineThreads)
                                           : renderGraphInterleaved(graph, sr, channels, totalFrames);
      }
    }

    // Auto-name WAV if requested and no explicit path was provided (or placeholder present)
    if (autoWavName && (wavPath.empty() || wavPath == "__AUTO__")) {
      std::string base = "render";
      if (!graphPath.empty()) {
        // derive basename from graphPath
        const std::string& p = graphPath;
        size_t slash = p.find_last_of("/\\");
        const std::string fname = (slash == std::string::npos) ? p : p.substr(slash + 1);
        size_t dot = fname.find_last_of('.');
        base = (dot == std::string::npos) ? fname : fname.substr(0, dot);
      }
      char buf[256];
      std::snprintf(buf, sizeof(buf), "%s_%lluf.wav", base.c_str(), (unsigned long long)totalFrames);
      wavPath = std::string(buf);
    }

    AudioFileSpec spec;
    spec.format = outFormat;
    spec.bitDepth = pcm16 ? BitDepth::Pcm16 : outDepth;
    spec.sampleRate = sr;
    spec.channels = channels;

    try {
      // Optional normalization
      double prePeakDb = 0.0, preRmsDb = 0.0;
      computePeakAndRms(interleaved, channels, prePeakDb, preRmsDb);
      double appliedGainDb = 0.0;
      if (doNormalize && std::isfinite(prePeakDb)) {
        appliedGainDb = peakTargetDb - prePeakDb;
        const double g = std::pow(10.0, appliedGainDb / 20.0);
        for (auto& s : interleaved) s = static_cast<float>(static_cast<double>(s) * g);
      }
      writeWithExtAudioFile(wavPath, spec, interleaved);
      double peakDb = 0.0, rmsDb = 0.0;
      computePeakAndRms(interleaved, channels, peakDb, rmsDb);
      const double seconds = static_cast<double>(totalFrames) / static_cast<double>(sr);
      const std::string hhmmss = formatDuration(seconds);
      const double nyquist = static_cast<double>(sr) * 0.5;
      std::fprintf(stderr,
                   "Exported %s\n  Frames: %llu\n  Duration: %s (%.3fs)\n  Sample rate: %u Hz (Nyquist %.1f Hz)\n  Channels: %u\n  Format: %s / %s\n  Peak: %.2f dBFS (pre: %.2f dBFS, gain: %+0.2f dB)\n  RMS: %.2f dBFS\n  Preroll: %.3f ms\n",
                   wavPath.c_str(), static_cast<unsigned long long>(totalFrames), hhmmss.c_str(), seconds,
                   sr, nyquist, channels, toStr(spec.format), toStr(spec.bitDepth), peakDb, prePeakDb, appliedGainDb, rmsDb,
                   prerollMsForSummary);
      if (printSha1 && !interleaved.empty()) {
        const std::string h = computeSha1Hex(interleaved.data(), interleaved.size() * sizeof(float));
        std::fprintf(stderr, "SHA1(samples): %s\n", h.c_str());
      }
      if (printMeters) {
        // duplicate a concise meters line for easy parsing
        std::fprintf(stderr, "Meters: peak_dBFS=%.2f rms_dBFS=%.2f\n", peakDb, rmsDb);
      }
      if (metersPerNode) {
        const auto nodeMeters = graph.getNodeMeters(channels);
        for (const auto& m : nodeMeters) {
          const bool inactive = (!std::isfinite(m.peakDb) && !std::isfinite(m.rmsDb));
          if (inactive) {
            std::fprintf(stderr, "Node %s: inactive\n", m.id.c_str());
          } else {
            std::fprintf(stderr, "Node %s: peak=%.2f dBFS rms=%.2f dBFS\n", m.id.c_str(), m.peakDb, m.rmsDb);
          }
        }
      }
      if (cpuStats || cpuStatsPerNode) {
        const auto s = graph.getCpuSummary();
        std::fprintf(stderr, "CPU block avg=%.3fms max=%.3fms (avg=%.1f%% max=%.1f%%) xruns=%llu blocks=%llu\n",
                     s.avgMs, s.maxMs, s.avgPercent, s.maxPercent,
                     static_cast<unsigned long long>(s.overruns), static_cast<unsigned long long>(s.blocks));
        if (cpuStatsPerNode) {
          const auto per = graph.getPerNodeCpu();
          for (const auto& n : per) std::fprintf(stderr, "  %s: avg=%.1fus max=%.1fus\n", n.id.c_str(), n.avgUs, n.maxUs);
        }
      }
    } catch (const std::exception& e) {
      std::fprintf(stderr, "Audio file write failed: %s\n", e.what());
      return 1;
    }
    graph.flushTrace();
    return 0;
  }

  // The graph and owned schedule outlive the queue, renderer and feeder.
  Graph graph;
  GraphSpec realtimeSpec;
  if (rackPath.size() && graphPath.empty()) graphPath = rackPath;
  if (!graphPath.empty()) {
    std::fprintf(stderr, "[rt-graph] starting: path=%s\n", graphPath.c_str());
    try {
      if (schemaStrict) {
        std::string diag;
        const int status = validateJsonWithDraft2020(graphPath, "docs/schema.graph.v1.json", diag);
        if (status != 0) {
          std::fprintf(stderr, "Schema validation failed: %s\n", diag.c_str());
          return 1;
        }
      }
      realtimeSpec = loadGraphSpecFromJsonFile(graphPath);
      const auto& spec = realtimeSpec;
      warnDrySuppression(spec);
      if (randomSeedOverride != 0) setGlobalSeed(randomSeedOverride);
      else if (spec.randomSeed != 0) setGlobalSeed(spec.randomSeed);
      for (const auto& ns : spec.nodes) {
        auto node = createNodeFromSpec(ns);
        if (node) graph.addNode(ns.id, std::move(node));
      }
      if (spec.hasMixer) {
        std::vector<MixerChannel> channels;
        for (const auto& input : spec.mixer.inputs) {
          MixerChannel channel;
          channel.id = input.id;
          channel.gain = input.gainPercent * 0.01f;
          channels.push_back(channel);
        }
        graph.setMixer(std::make_unique<MixerNode>(std::move(channels),
            spec.mixer.masterPercent * 0.01f, spec.mixer.softClip));
      }
      if (!spec.connections.empty()) graph.setConnections(spec.connections);
      graph.setPortDescriptors(spec.nodes);
      if (metersPerNode) graph.enableStats(true);
      if (cpuStats || cpuStatsPerNode) graph.enableCpuStats(true);
    } catch (const std::exception& error) {
      std::fprintf(stderr, "Failed to load graph JSON: %s\n", error.what());
      return 1;
    }
  } else {
    graph.addNode("kick_default", std::make_unique<KickNode>(params));
  }

  std::unique_ptr<mam::session::EventSchedule> schedule;
  SpscCommandQueue<2048> commandQueue;
  RealtimeGraphRenderer rt;
  // Destruction requests cancellation and joins before the renderer, queue,
  // schedule and graph are destroyed, including exception paths.
  std::jthread feeder;
  try {
    rt.setCommandQueue(&commandQueue);
    rt.setCmdQueueDebug(rtDebugFeed);
    const double diagnosticBpm = realtimeSpec.hasTransport && realtimeSpec.transport.bpm > 0.0
        ? realtimeSpec.transport.bpm : 120.0;
    const uint32_t diagnosticResolution = realtimeSpec.hasTransport
        ? realtimeSpec.transport.resolution : 16u;
    rt.setDiagnostics(printTriggers, diagnosticBpm, diagnosticResolution);
    rt.setTransportEmitEnabled(false);
    rt.start(graph, 48000.0, 2); // Prepare the device; no consumer is running yet.
    auto plan = mam::app::planGraphPlayback(realtimeSpec, static_cast<uint32_t>(rt.sampleRate() + 0.5));
    const SampleTime loopFrames = plan.loopFrames;
    rt.setDiagLoop(loopFrames);
    schedule = std::make_unique<mam::session::EventSchedule>(std::move(plan.events));
    const auto initialFrames = static_cast<SampleTime>(3.0 * rt.sampleRate());
    const auto primed = mam::session::primeSessionQueue(*schedule, commandQueue, initialFrames,
        [] { return gRunning.load(std::memory_order_relaxed); });
    if (primed.stopped || !gRunning.load(std::memory_order_relaxed)) return 0;
    if (printLatency) {
      const auto preroll = computeGraphPrerollSamples(realtimeSpec, static_cast<uint32_t>(rt.sampleRate() + 0.5));
      std::fprintf(stderr, "Graph preroll: %.3f ms\n", 1000.0 * static_cast<double>(preroll) / rt.sampleRate());
    }
    rt.begin();
    const auto refillFrames = static_cast<SampleTime>(5.0 * rt.sampleRate());
    if (schedule->nextFrame()) {
      feeder = std::jthread([&, refillFrames](std::stop_token stop) {
        const auto running = [&] {
          return !stop.stop_requested() && gRunning.load(std::memory_order_relaxed);
        };
        while (running()) {
          const auto result = schedule->pump(mam::session::lookaheadEnd(rt.sampleCounter(), refillFrames),
              [&](const Command& command) { return commandQueue.push(command); },
              commandQueue.capacity(), running);
          if (result.stopped || !result.pending) break;
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
      });
    }
    uint64_t lastPrintedLoop = 0;
    uint64_t lastLateCount = 0;
    while (gRunning.load(std::memory_order_relaxed)) {
      if (isStdinReady()) {
        char buffer[4];
        (void)read(STDIN_FILENO, buffer, sizeof(buffer));
        gRunning.store(false, std::memory_order_relaxed);
        break;
      }
      const SampleTime frames = rt.sampleCounter();
      if (loopFrames > 0 && !printTriggers && frames / loopFrames > lastPrintedLoop) {
        lastPrintedLoop = frames / loopFrames;
        if (verbose) {
          const double seconds = static_cast<double>(frames) / rt.sampleRate();
          std::fprintf(stderr, "Loop %llu at %s (%.3fs)\n",
              static_cast<unsigned long long>(lastPrintedLoop), formatDuration(seconds).c_str(), seconds);
        }
      }
      const auto late = rt.lateEventCount();
      if (late != lastLateCount) {
        std::fprintf(stderr, "[rt-graph] late events delivered: %llu\n", static_cast<unsigned long long>(late));
        lastLateCount = late;
      }
      if (quitAfterSec > 0.0 && static_cast<double>(frames) / rt.sampleRate() >= quitAfterSec) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    feeder.request_stop();
    if (feeder.joinable()) feeder.join();
    rt.stop();
    // Mutable node/graph statistics are read only after the consumer stops.
    if (cpuStats || cpuStatsPerNode) {
      const auto stats = graph.getCpuSummary();
      std::fprintf(stderr, "CPU block avg=%.3fms max=%.3fms (avg=%.1f%% max=%.1f%%) xruns=%llu blocks=%llu\n",
          stats.avgMs, stats.maxMs, stats.avgPercent, stats.maxPercent,
          static_cast<unsigned long long>(stats.overruns), static_cast<unsigned long long>(stats.blocks));
      if (cpuStatsPerNode) {
        for (const auto& node : graph.getPerNodeCpu())
          std::fprintf(stderr, "  %s: avg=%.1fus max=%.1fus\n", node.id.c_str(), node.avgUs, node.maxUs);
      }
    }
    if (metersPerNode) {
      for (const auto& meter : graph.getNodeMeters(2))
        std::fprintf(stderr, "Node %s: peak=%.2f dBFS rms=%.2f dBFS\n",
            meter.id.c_str(), meter.peakDb, meter.rmsDb);
    }
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Realtime graph failed: %s\n", error.what());
    return 1;
  }
  return 0;
}
