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
#include "../session/SessionFeeding.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <thread>
#include <unordered_map>
#include <unistd.h>

namespace mam::app {
int runRealtimeSession(const CliOptions& options, std::atomic<bool>& running,
                       bool (*stdinReady)()) {
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
    // Destruction order is intentional: audio stops before borrowed command
    // strings, queue storage or graphs can be destroyed on any exit path.
    std::unique_ptr<mam::session::EventSchedule> schedule;
    SpscCommandQueue<16384> cmdQueue;
    RealtimeSessionRenderer srt;
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
    std::vector<mam::session::ScheduledEvent> sessionEvents;
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

        mam::session::ScheduledEvent cmd{};
        cmd.frame = static_cast<uint64_t>(std::llround(resolvedTimeSec * srt.sampleRate()));
        cmd.nodeId = sc.nodeId;
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
        if (!sc.paramName.empty()) cmd.paramName = sc.paramName;
        cmd.source = 1; // session
        sessionEvents.push_back(std::move(cmd));
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

    std::vector<mam::session::ScheduledEvent> events;
    for (const auto& rack : rackRTs) {
      for (const auto& command : rack.baseCmds) {
        mam::session::ScheduledEvent event;
        event.frame = command.sampleTime;
        event.nodeId = command.nodeId;
        if (command.type == "Trigger") event.type = CommandType::Trigger;
        else if (command.type == "SetParam") event.type = CommandType::SetParam;
        else if (command.type == "SetParamRamp") event.type = CommandType::SetParamRamp;
        else throw std::invalid_argument("Unknown realtime rack command type: " + command.type);
        event.paramId = command.paramId;
        event.value = command.value;
        event.rampMs = command.rampMs;
        event.paramName = command.paramName;
        event.periodFrames = rack.loopLen; // No transport means one-shot, not three copies.
        events.push_back(std::move(event));
      }
    }
    events.insert(events.end(), std::make_move_iterator(sessionEvents.begin()),
                  std::make_move_iterator(sessionEvents.end()));
    schedule = std::make_unique<mam::session::EventSchedule>(std::move(events));
    const SampleTime lookahead = static_cast<SampleTime>(3.0 * srt.sampleRate());
    const auto initial = mam::session::primeSessionQueue(*schedule, cmdQueue, lookahead,
        [&] { return running.load(); });
    if (initial.stopped || !running.load()) return 0;
    std::fprintf(stderr, "[rt-session] initial horizon enqueued=%zu capacity=%zu\n",
                 initial.enqueued, cmdQueue.capacity());
    srt.begin();
    // One producer owns the cursor after startup. Never wait inside a pump or
    // restart loop indices; full queues leave the earliest event pending.
    std::jthread feeder([&](std::stop_token stop) {
      const auto keepRunning = [&] { return running.load() && !stop.stop_requested(); };
      while (keepRunning()) {
        const auto end = mam::session::lookaheadEnd(srt.sampleCounter(), lookahead);
        schedule->pump(end, [&](const Command& event) { return cmdQueue.push(event); },
                       cmdQueue.capacity(), keepRunning);
        if (keepRunning()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    });
    // Wait loop with optional quit-after
    const double sr = srt.sampleRate();
    uint64_t reportedLate = 0;
    while (running.load()) {
      const auto late = srt.lateEventCount();
      if (late != reportedLate) {
        std::fprintf(stderr, "[rt-session] late events applied at block start: %llu total\n",
                     static_cast<unsigned long long>(late));
        reportedLate = late;
      }
      if (quitAfterSec > 0.0) { const double t = static_cast<double>(srt.sampleCounter()) / sr; if (t >= quitAfterSec) { running.store(false); break; } }
      if (stdinReady()) { char buf[4]; (void)read(STDIN_FILENO, buf, sizeof(buf)); running.store(false); break; }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    feeder.request_stop();
    feeder.join();
    srt.stop();
    if (srt.lateEventCount() != reportedLate)
      std::fprintf(stderr, "[rt-session] late events applied at block start: %llu total\n",
                   static_cast<unsigned long long>(srt.lateEventCount()));
    return 0;
  } catch (const std::exception& e) { std::fprintf(stderr, "Realtime session failed: %s\n", e.what()); return 1; }
}
} // namespace mam::app
