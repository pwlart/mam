#include "SessionPlayback.hpp"
#include "../core/Graph.hpp"
#include "../core/GraphUtils.hpp"
#include "../core/NodeFactory.hpp"
#include "../core/MixerNode.hpp"
#include "../core/ParamMap.hpp"
#include "../offline/TransportGenerator.hpp"
#include "../realtime/RealtimeSessionRenderer.hpp"
#include "../session/SessionSpec.hpp"
#include "../session/RackTimeline.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <queue>
#include <thread>
#include <unordered_map>
#include <unistd.h>

namespace mam::app {
int runRealtimeSession(const CliOptions& options, std::atomic<bool>& running,
                       bool (*stdinReady)(), const char* (*internNodeId)(const std::string&)) {
  const auto& sessionPath = options.sessionPath;
  const auto& offlineSr = options.offlineSr;
  const auto& quitAfterSec = options.quitAfterSec;
  const auto& printMeters = options.printMeters;
  const auto& metersIntervalSec = options.metersIntervalSec;
  const auto& metersPerNode = options.metersPerNode;
  const auto& metricsNdjsonPath = options.metricsNdjsonPath;
  const auto& metricsScopeRacks = options.metricsScopeRacks;
  const auto& metricsScopeBuses = options.metricsScopeBuses;
  const auto& rtDebugSession = options.rtDebugSession;
  const auto& printTriggers = options.printTriggers;
  const auto& printLatency = options.printLatency;
  // Resolve and validate session file
  std::fprintf(stderr, "[rt-session] resolved path=%s\n", sessionPath.c_str());
  {
    std::ifstream sf(sessionPath);
    if (!sf.good()) { std::fprintf(stderr, "Session file not found: %s\n", sessionPath.c_str()); return 1; }
  }
  try {
    SessionSpec sess = loadSessionSpecFromJsonFile(sessionPath);
    // Build per-rack graphs with prefixed node ids, synthesize base commands for one loop, compute loopLen
    std::vector<std::unique_ptr<Graph>> graphsOwned;
    using RackRT = mam::session::RackTimeline;
    std::vector<RackRT> rackRTs;
    // Global map: full node id (rack:node) -> node type (e.g., "kick") for session command param-name mapping
    std::unordered_map<std::string, std::string> typeByFullNodeId;
    const uint32_t sessionSrU32 = static_cast<uint32_t>((offlineSr > 0.0 ? offlineSr : 48000.0) + 0.5);
    std::fprintf(stderr, "[rt-session] starting: racks=%zu sr=%u\n", sess.racks.size(), sessionSrU32);
    // Determine solo/mute policy once
    bool anySoloGlobal = false; for (const auto& rr : sess.racks) if (rr.solo) { anySoloGlobal = true; break; }
    for (const auto& rr : sess.racks) {
      std::fprintf(stderr, "[rt-session] rack-config id=%s muted=%s solo=%s\n", rr.id.c_str(), rr.muted?"true":"false", rr.solo?"true":"false");
      GraphSpec gs = loadGraphSpecFromJsonFile(rr.path);
      auto g = std::make_unique<Graph>();
      for (const auto& ns : gs.nodes) {
        auto node = createNodeFromSpec(ns);
        if (node) g->addNode(rr.id + ":" + ns.id, std::move(node));
        // Accumulate type mapping for session-level param-name resolution
        typeByFullNodeId.emplace(rr.id + ":" + ns.id, ns.type);
      }
      if (gs.hasMixer) {
        std::vector<MixerChannel> chans;
        for (const auto& inp : gs.mixer.inputs) { MixerChannel mc; mc.id = rr.id + ":" + inp.id; mc.gain = inp.gainPercent * (1.0f/100.0f); chans.push_back(mc);}
        const float master = gs.mixer.masterPercent * (1.0f/100.0f);
        g->setMixer(std::make_unique<MixerNode>(std::move(chans), master, gs.mixer.softClip));
      }
      if (!gs.connections.empty()) {
        auto conns = gs.connections; for (auto& c : conns) { c.from = rr.id + ":" + c.from; c.to = rr.id + ":" + c.to; }
        g->setConnections(conns);
      }
      const bool activeRack = anySoloGlobal ? rr.solo : !rr.muted;
      std::fprintf(stderr, "[rt-session] rack-active id=%s active=%s (anySolo=%s)\n", rr.id.c_str(), activeRack?"true":"false", anySoloGlobal?"true":"false");
      const auto timeline = mam::session::planRackTimeline(gs, rr, sessionSrU32, activeRack);
      const auto& cmds = timeline.baseCmds;
      const auto loopLen = timeline.loopLen;
      if (rtDebugSession) {
        size_t preview = std::min<size_t>(cmds.size(), 12);
        for (size_t i = 0; i < preview; ++i) {
          const auto& c = cmds[i];
          std::fprintf(stderr, "[rt-session] preview cmd[%zu]: t=%.3fs type=%s node=%s\n",
            i, static_cast<double>(c.sampleTime) / static_cast<double>(sessionSrU32), c.type.c_str(), c.nodeId.c_str());
        }
      }
      rackRTs.push_back(timeline);
      graphsOwned.push_back(std::move(g));
      std::fprintf(stderr, "[rt-session] rack=%s cmds=%zu loopLen=%llu\n", rr.id.c_str(), cmds.size(), (unsigned long long)loopLen);
    }
    if (rackRTs.empty()) { std::fprintf(stderr, "[rt-session] error: no racks\n"); return 1; }
    size_t totalCmds = 0; for (const auto& r : rackRTs) totalCmds += r.baseCmds.size(); if (totalCmds == 0) std::fprintf(stderr, "[rt-session] error: synthesized zero commands across racks\n");
    // Start realtime renderer
    RealtimeSessionRenderer srt; SpscCommandQueue<16384> cmdQueue;
    srt.setCommandQueue(&cmdQueue); srt.setDiagnostics(printTriggers); srt.setDebug(rtDebugSession); srt.setMeters(printMeters || metersPerNode, metersIntervalSec);
    if (!metricsNdjsonPath.empty()) srt.setMetricsNdjson(metricsNdjsonPath.c_str(), metricsScopeRacks, metricsScopeBuses);
    // Configure session xfaders (if any)
    {
      std::vector<RealtimeSessionRenderer::Rack> cfg; cfg.reserve(graphsOwned.size());
      for (size_t i = 0; i < graphsOwned.size(); ++i) cfg.push_back(RealtimeSessionRenderer::Rack{graphsOwned[i].get(), sess.racks[i].id, sess.racks[i].gain, sess.racks[i].muted, sess.racks[i].solo});
      srt.setXfaders(sess.xfaders, cfg);
    }
    std::vector<RealtimeSessionRenderer::Rack> rracks; rracks.reserve(graphsOwned.size()); for (size_t i = 0; i < graphsOwned.size(); ++i) rracks.push_back(RealtimeSessionRenderer::Rack{graphsOwned[i].get(), sess.racks[i].id, sess.racks[i].gain, sess.racks[i].muted, sess.racks[i].solo});
    const double rtSr = offlineSr > 0.0 ? offlineSr : 48000.0;
    if (printLatency) {
      uint64_t totalPreroll = 0;
      for (const auto& r : rackRTs) {
        // Estimate per-rack preroll using each rack's spec via type map size as proxy is not possible here;
        // reuse computeGraphPrerollSamples by reloading spec for path
        auto it = std::find_if(sess.racks.begin(), sess.racks.end(), [&](const auto& rx){ return rx.id == r.rackId; });
        if (it != sess.racks.end()) {
          try {
            GraphSpec gsLat = loadGraphSpecFromJsonFile(it->path);
            totalPreroll = std::max<uint64_t>(totalPreroll, computeGraphPrerollSamples(gsLat, static_cast<uint32_t>(rtSr + 0.5)));
          } catch (...) {}
        }
      }
      std::fprintf(stderr, "Session preroll (max rack): %.3f ms\n", 1000.0 * static_cast<double>(totalPreroll) / rtSr);
    }
    srt.start(rracks, sess.buses, sess.routes, rtSr, 2);
    // Collect session-level commands (absolute time or musical time, realtime) for initial enqueue
    std::vector<Command> sessionInitCmds;
    if (!sess.commands.empty()) {
      // Use typeByFullNodeId from specs for param name resolution (prefixed ids)
      auto mapParamId = [](const std::string& type, const std::string& name) -> uint16_t {
        if (type == std::string("kick")) return resolveParamIdByName(kKickParamMap, name);
        if (type == std::string("clap")) return resolveParamIdByName(kClapParamMap, name);
        if (type == std::string("tb303_ext")) return resolveParamIdByName(kTb303ParamMap, name);
        if (type == std::string("mam_chip")) return resolveParamIdByName(kMamChipParamMap, name);
        return 0;
      };
      for (const auto& sc : sess.commands) {
        double resolvedTimeSec = sc.timeSec;

        // Resolve musical time if absolute time not provided
        if (sc.timeSec == 0.0 && !sc.rack.empty() && sc.bar > 0) {
          // Find the referenced rack
          auto rackIt = std::find_if(sess.racks.begin(), sess.racks.end(),
            [&](const SessionSpec::RackRef& r) { return r.id == sc.rack; });
          if (rackIt == sess.racks.end()) {
            std::fprintf(stderr, "[rt-session] warning: musical command references unknown rack '%s'\n", sc.rack.c_str());
            continue;
          }

          // Find the corresponding rack runtime info
          auto rackRtIt = std::find_if(rackRTs.begin(), rackRTs.end(),
            [&](const RackRT& r) { return r.rackId == sc.rack; });
          if (rackRtIt == rackRTs.end()) {
            std::fprintf(stderr, "[rt-session] warning: no runtime info for rack '%s'\n", sc.rack.c_str());
            continue;
          }

          // Convert musical time to seconds using the rack's transport
          const uint32_t bar = sc.bar - 1; // Convert to 0-based
          const uint32_t step = (sc.step > 0) ? sc.step - 1 : 0; // Convert to 0-based, default to bar start
          const uint32_t stepsPerBar = sc.res;

          if (rackRtIt->framesPerBar > 0) {
            const double secPerBar = static_cast<double>(rackRtIt->framesPerBar) / srt.sampleRate();
            const double stepsPerSec = static_cast<double>(stepsPerBar) / secPerBar;
            const double stepSec = static_cast<double>(step) / stepsPerSec;

            resolvedTimeSec = static_cast<double>(bar) * secPerBar + stepSec;
            std::fprintf(stderr, "[rt-session] resolved musical command: rack=%s bar=%u step=%u res=%u -> %.3f sec\n",
              sc.rack.c_str(), sc.bar, sc.step, sc.res, resolvedTimeSec);
          } else {
            std::fprintf(stderr, "[rt-session] warning: rack '%s' has zero loop length, cannot resolve musical time\n", sc.rack.c_str());
            continue;
          }
        } else if (sc.timeSec > 0.0 && (!sc.rack.empty() || sc.bar > 0)) {
          std::fprintf(stderr, "[rt-session] warning: command has both timeSec and musical fields, using timeSec\n");
        }

        Command cmd{};
        cmd.sampleTime = static_cast<uint64_t>(std::llround(resolvedTimeSec * srt.sampleRate()));
        cmd.nodeId = internNodeId(sc.nodeId);
        cmd.type = (sc.type == std::string("SetParam")) ? CommandType::SetParam : CommandType::SetParam;
        // Resolve parameter id from name (if provided)
        uint16_t pid = 0;
        if (!sc.paramName.empty()) {
          auto it = typeByFullNodeId.find(sc.nodeId);
          const std::string nodeType = (it != typeByFullNodeId.end()) ? it->second : std::string();
          pid = mapParamId(nodeType, sc.paramName);
        }
        cmd.paramId = pid;
        cmd.value = sc.value;
        cmd.rampMs = sc.rampMs;
        if (!sc.paramName.empty()) cmd.paramNameStr = internNodeId(sc.paramName);
        cmd.source = 1; // session
        sessionInitCmds.push_back(cmd);
      }
    }
    // Adjust pre-synthesized command timing to actual device sample rate if needed
    {
      const uint32_t srActual = static_cast<uint32_t>(srt.sampleRate() + 0.5);
      if (srActual != sessionSrU32) {
        const double scale = static_cast<double>(srActual) / static_cast<double>(sessionSrU32);
        for (auto& r : rackRTs) {
          for (auto& c : r.baseCmds) {
            c.sampleTime = static_cast<uint64_t>(std::llround(static_cast<double>(c.sampleTime) * scale));
          }
          r.loopLen = static_cast<uint64_t>(std::llround(static_cast<double>(r.loopLen) * scale));
        }
        std::fprintf(stderr, "[rt-session] rescaled commands to device sr=%u (scale=%.6f)\n", srActual, scale);
      }
    }
    // Optional: align transports (shift racks so the earliest first trigger begins at t=0)
    if (sess.alignTransports) {
      uint64_t earliest = UINT64_MAX;
      std::vector<uint64_t> firsts(rackRTs.size(), UINT64_MAX);
      for (size_t i = 0; i < rackRTs.size(); ++i) {
        for (const auto& c : rackRTs[i].baseCmds) {
          if (c.type == std::string("Trigger")) { firsts[i] = c.sampleTime; break; }
        }
        if (firsts[i] < earliest) earliest = firsts[i];
      }
      if (earliest != UINT64_MAX) {
        for (size_t i = 0; i < rackRTs.size(); ++i) {
          if (firsts[i] == UINT64_MAX) continue;
          const int64_t shift = static_cast<int64_t>(firsts[i]) - static_cast<int64_t>(earliest);
          if (shift > 0) {
            const uint64_t u = static_cast<uint64_t>(shift);
            for (auto& c : rackRTs[i].baseCmds) c.sampleTime -= std::min(c.sampleTime, u);
          }
        }
      }
    }

    // Initial enqueue (globally time-sorted merge across racks + session-level cmds)
    {
      // Determine active racks for enqueue (skip muted; if any solo, only solo)
      bool anySolo = false; for (const auto& rr : sess.racks) if (rr.solo) { anySolo = true; break; }
      std::vector<bool> activeFlags; activeFlags.reserve(sess.racks.size());
      for (const auto& rr : sess.racks) activeFlags.push_back(anySolo ? rr.solo : !rr.muted);
      // Build combined list
      std::vector<Command> combined; combined.reserve(sessionInitCmds.size() + 1024);
      // Rack events (seed initial loop and a small horizon of extra loops to avoid early gaps)
      const uint32_t initAheadLoops = 2;
      for (size_t i = 0; i < rackRTs.size(); ++i) if (i < activeFlags.size() && activeFlags[i]) {
        const uint64_t ll = rackRTs[i].loopLen;
        for (uint32_t rep = 0; rep < 1u + initAheadLoops; ++rep) {
          const uint64_t offset = (ll > 0) ? (rep * ll) : 0ull;
          for (const auto& c : rackRTs[i].baseCmds) {
            Command cmd{}; cmd.sampleTime = c.sampleTime + offset; cmd.nodeId = internNodeId(c.nodeId);
            cmd.type = (c.type == std::string("Trigger")) ? CommandType::Trigger : (c.type == std::string("SetParam")) ? CommandType::SetParam : CommandType::SetParamRamp;
            cmd.paramId = c.paramId; cmd.value = c.value; cmd.rampMs = c.rampMs;
            if (!c.paramName.empty()) cmd.paramNameStr = internNodeId(c.paramName);
            cmd.source = 0;
            combined.push_back(cmd);
          }
        }
      }
      // Session-level
      combined.insert(combined.end(), sessionInitCmds.begin(), sessionInitCmds.end());
      // Sort globally: by time, then nodeId, then type (Set before Trigger), then paramId
      std::stable_sort(combined.begin(), combined.end(), [](const Command& a, const Command& b){
        if (a.sampleTime != b.sampleTime) return a.sampleTime < b.sampleTime;
        const char* an = a.nodeId ? a.nodeId : ""; const char* bn = b.nodeId ? b.nodeId : "";
        int cmp = std::strcmp(an, bn); if (cmp != 0) return cmp < 0;
        auto ord = [](CommandType t){ return (t == CommandType::SetParam) ? 0 : (t == CommandType::SetParamRamp ? 1 : 2); };
        if (ord(a.type) != ord(b.type)) return ord(a.type) < ord(b.type);
        if (a.paramId != b.paramId) return a.paramId < b.paramId;
        return a.value < b.value;
      });
      size_t totalPushed = 0; for (const auto& ev : combined) { while (running.load() && !cmdQueue.push(ev)) std::this_thread::sleep_for(std::chrono::milliseconds(1)); if (!running.load()) break; totalPushed++; }
      std::fprintf(stderr, "[rt-session] init enqueued total cmds=%zu (combined)\n", totalPushed);
    }
    // Start audio after initial enqueue to ensure first triggers are applied in the very first block
    srt.begin();
    // Feeder thread
    std::thread feeder([&cmdQueue, &rackRTs, &srt, &running, internNodeId, sess]() mutable {
      const uint64_t desiredAhead = static_cast<uint64_t>(3.0 * srt.sampleRate());
      std::vector<uint64_t> nextOffset(rackRTs.size(), 0ull); for (size_t i = 0; i < rackRTs.size(); ++i) nextOffset[i] = rackRTs[i].loopLen;
      bool anySolo = false; for (const auto& rr : sess.racks) if (rr.solo) { anySolo = true; break; }
      std::vector<bool> activeFlags; activeFlags.reserve(sess.racks.size());
      for (const auto& rr : sess.racks) activeFlags.push_back(anySolo ? rr.solo : !rr.muted);
      while (running.load()) {
        const uint64_t now = static_cast<uint64_t>(srt.sampleCounter());
        // collect eligible racks
        std::vector<size_t> elig; elig.reserve(rackRTs.size());
        for (size_t i = 0; i < rackRTs.size(); ++i) if (i < activeFlags.size() && activeFlags[i]) { if (rackRTs[i].loopLen > 0 && nextOffset[i] <= now + desiredAhead) elig.push_back(i); }
        if (!elig.empty()) {
          struct Item { uint64_t t; size_t ri; size_t ci; };
          auto cmp = [](const Item& a, const Item& b){ return a.t > b.t; };
          std::priority_queue<Item, std::vector<Item>, decltype(cmp)> pq(cmp);
          for (size_t idx : elig) {
            if (!rackRTs[idx].baseCmds.empty()) pq.push(Item{rackRTs[idx].baseCmds[0].sampleTime + nextOffset[idx], idx, 0});
          }
          while (!pq.empty() && running.load()) {
            Item it = pq.top(); pq.pop(); const auto& c = rackRTs[it.ri].baseCmds[it.ci];
            Command cmd{}; cmd.sampleTime = c.sampleTime + nextOffset[it.ri]; cmd.nodeId = internNodeId(c.nodeId); cmd.type = (c.type == std::string("Trigger")) ? CommandType::Trigger : (c.type == std::string("SetParam")) ? CommandType::SetParam : CommandType::SetParamRamp; cmd.paramId = c.paramId; cmd.value = c.value; cmd.rampMs = c.rampMs; if (!c.paramName.empty()) cmd.paramNameStr = internNodeId(c.paramName); cmd.source = 0;
            while (running.load() && !cmdQueue.push(cmd)) std::this_thread::sleep_for(std::chrono::milliseconds(1)); if (!running.load()) break;
            const size_t nextCi = it.ci + 1; if (nextCi < rackRTs[it.ri].baseCmds.size()) pq.push(Item{rackRTs[it.ri].baseCmds[nextCi].sampleTime + nextOffset[it.ri], it.ri, nextCi});
          }
          for (size_t idx : elig) nextOffset[idx] += rackRTs[idx].loopLen;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    });
    // Wait loop with optional quit-after
    const double sr = srt.sampleRate();
    while (running.load()) {
      if (quitAfterSec > 0.0) { const double t = static_cast<double>(srt.sampleCounter()) / sr; if (t >= quitAfterSec) { running.store(false); break; } }
      if (stdinReady()) { char buf[4]; (void)read(STDIN_FILENO, buf, sizeof(buf)); running.store(false); break; }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (feeder.joinable()) feeder.join(); srt.stop();
    return 0;
  } catch (const std::exception& e) { std::fprintf(stderr, "Realtime session failed: %s\n", e.what()); return 1; }
}
} // namespace mam::app
