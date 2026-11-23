#pragma once

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudioTypes.h>
#include <vector>
#include <atomic>
#include <algorithm>
#include <unordered_map>
#include <cstring>
#include <string>
#include <string_view>
#include <span>
#include <memory>
#include <limits>
#include <cstdio>
#include <chrono>
#include <cmath>
#include "../core/Graph.hpp"
#include "../core/ScopedAudioUnit.hpp"
#include "../core/OsStatusUtils.hpp"
#include "../core/Command.hpp"
#include "../core/ParamMap.hpp"
#include "../session/SessionSpec.hpp"
#include "../session/SessionFeeding.hpp"
#include "../core/SpectralDuckerNode.hpp"
#include "RealtimeTelemetry.hpp"

class RealtimeSessionRenderer {
public:
  enum class RenderStatus { Ok, NotPrepared, InvalidLayout, CapacityExceeded, InvalidContext, NodeFailure };
  struct Rack { Graph* graph=nullptr; std::string id; float gain=1.0f; bool muted=false; bool solo=false; };

  RealtimeSessionRenderer() = default;
  ~RealtimeSessionRenderer() { stop(); }

  // All configuration/preparation calls are control-thread, stopped-only calls.
  // Graphs, the command queue and queued target strings outlive playback and
  // the final telemetry poll. The callback never changes routing ownership.
  template <size_t N>
  void setCommandQueue(SpscCommandQueue<N>* queue) {
    requireStopped();
    pollDiagnostics();
    prepared_ = false;
    cmdQueue_ = queue;
    eventCapacity_ = queue ? queue->capacity() : 0;
    queueDrain_ = [](void* pointer, SampleTime cutoff, std::span<Command> output) noexcept {
      return static_cast<SpscCommandQueue<N>*>(pointer)->drainUpToBounded(cutoff, output);
    };
  }
  void setDiagnostics(bool enabled) { requireStopped(); printTriggers_ = enabled; }
  void setDebug(bool enabled) { requireStopped(); debug_ = enabled; }
  void setMeters(bool enabled, double intervalSec) {
    requireStopped(); metersEnabled_ = enabled;
    metersIntervalSec_ = (std::isfinite(intervalSec) && intervalSec > .05) ? intervalSec : 1.0;
  }
  void setMetricsNdjson(const char* path, bool includeRacks, bool includeBuses) {
    requireStopped();
    pollDiagnostics();
    metricsIncludeRacks_ = includeRacks; metricsIncludeBuses_ = includeBuses;
    if (metricsFile_) { std::fclose(metricsFile_); metricsFile_ = nullptr; }
    metricsEnabled_ = false;
    if (path && *path) {
      metricsFile_ = std::fopen(path, "w");
      metricsEnabled_ = metricsFile_ != nullptr;
    }
  }

  void setXfaders(const std::vector<SessionSpec::XfaderRef>& inputs, const std::vector<Rack>& racks) {
    requireStopped();
    pollDiagnostics();
    prepared_ = false;
    const auto indexOf = [&](const std::string& id) {
      for (size_t i = 0; i < racks.size(); ++i) if (racks[i].id == id) return i;
      return SIZE_MAX;
    };
    xfaders_.clear();
    for (const auto& input : inputs) {
      if (input.racks.size() < 2) continue;
      XfaderState state;
      state.id = input.id; state.aRackId = input.racks[0]; state.bRackId = input.racks[1];
      state.aIndex = indexOf(input.racks[0]); state.bIndex = indexOf(input.racks[1]);
      if (state.aIndex == SIZE_MAX || state.bIndex == SIZE_MAX) continue;
      if (!std::isfinite(input.smoothingMs) || !std::isfinite(input.lfo.freqHz) ||
          !std::isfinite(input.lfo.phase01))
        throw std::invalid_argument("Session crossfader settings must be finite");
      state.lawEqualPower = input.law != "linear";
      state.smoothingMs = input.smoothingMs;
      state.lfoEnabled = input.lfo.has; state.freqHz = input.lfo.freqHz;
      state.phase01 = boundedPhase(input.lfo.phase01);
      if (state.lfoEnabled) state.x = .5 * (std::sin(2.0 * M_PI * state.phase01) + 1.0);
      state.xTarget = state.x; state.initialX = state.x; state.initialPhase01 = state.phase01;
      xfaders_.push_back(std::move(state));
    }
  }

  // Device-free preparation shared by tests and the CoreAudio adapter below.
  // Actual node DSP contracts remain enforced by Graph::prepareRealtime.
  void prepareSession(const std::vector<Rack>& racks,
                      const std::vector<SessionSpec::BusRef>& buses,
                      const std::vector<SessionSpec::RouteRef>& routes,
                      double sampleRate, uint32_t maxFrames, uint32_t channels) {
    requireStopped();
    pollDiagnostics();
    prepared_ = false;
    if (!std::isfinite(sampleRate) || sampleRate <= 0 || maxFrames == 0 ||
        (channels != 1 && channels != 2))
      throw std::invalid_argument("Session preparation requires a finite rate, nonzero capacity and mono/stereo layout");
    if (static_cast<size_t>(maxFrames) > std::numeric_limits<size_t>::max() / channels ||
        eventCapacity_ > std::numeric_limits<size_t>::max() - 2)
      throw std::length_error("Session preparation capacity overflows");
    const double maxDuration = static_cast<double>(maxFrames) / sampleRate;
    if (!std::isfinite(maxDuration))
      throw std::invalid_argument("Session block duration overflows");
    for (const auto& xfader : xfaders_)
      if (xfader.lfoEnabled && !std::isfinite(xfader.freqHz * maxDuration))
        throw std::invalid_argument("Session LFO phase increment overflows");
    racks_ = racks; channels_ = channels; sampleRate_ = sampleRate; maxFrames_ = maxFrames;
    const size_t samples = static_cast<size_t>(maxFrames) * channels;
    rackScratch_.assign(racks.size(), std::vector<float>(samples, 0.0f));
    busScratch_.assign(buses.size(), std::vector<float>(samples, 0.0f));
    sidechainScratch_.assign(samples, 0.0f);
    eventStorage_.resize(eventCapacity_);
    splitStorage_.resize(eventCapacity_ + 2);
    rackGainMul_.resize(racks.size());
    rackMeters_.assign(racks.size(), Meter{}); busMeters_.assign(buses.size(), Meter{});
    activeRacks_.assign(racks.size(), false); rackHadRoute_.assign(racks.size(), false);
    busNames_.clear(); for (const auto& bus : buses) busNames_.push_back(bus.id);
    const bool anySolo = std::any_of(racks.begin(), racks.end(), [](const auto& rack) { return rack.solo; });
    nodeTargets_.clear(); nodeTypeById_.clear();
    for (size_t i = 0; i < racks_.size(); ++i) {
      const auto& rack = racks_[i];
      activeRacks_[i] = !rack.muted && (!anySolo || rack.solo);
      if (rack.graph) {
        rack.graph->prepareRealtime(sampleRate_, maxFrames_, channels_);
        rack.graph->reset();
        rack.graph->forEachNode([&](const std::string& id, Node& node) {
          nodeTargets_.push_back(NodeTarget{id, &node});
          nodeTypeById_[id] = node.name();
        });
      }
    }
    std::stable_sort(nodeTargets_.begin(), nodeTargets_.end(),
                     [](const auto& a, const auto& b) { return a.id < b.id; });
    const auto rackIndex = [&](const std::string& id) {
      for (size_t i = 0; i < racks_.size(); ++i) if (racks_[i].id == id) return i;
      return SIZE_MAX;
    };
    for (auto& xfader : xfaders_) {
      xfader.aIndex = rackIndex(xfader.aRackId); xfader.bIndex = rackIndex(xfader.bRackId);
      if (xfader.aIndex == SIZE_MAX || xfader.bIndex == SIZE_MAX)
        xfader.aIndex = xfader.bIndex = SIZE_MAX;
      xfader.phase01 = xfader.initialPhase01;
      xfader.x = xfader.xTarget = xfader.initialX;
      xfader.lastGA = xfader.lastGB = 1.0;
    }
    compiledRoutes_.clear();
    for (const auto& route : routes) {
      const size_t ri = rackIndex(route.from);
      if (ri == SIZE_MAX || !activeRacks_[ri]) continue;
      const auto bus = std::find(busNames_.begin(), busNames_.end(), route.to);
      if (bus == busNames_.end()) continue;
      compiledRoutes_.push_back({ri, static_cast<size_t>(bus - busNames_.begin()), route.gain});
      rackHadRoute_[ri] = true;
    }
    inserts_.clear();
    for (size_t bi = 0; bi < buses.size(); ++bi) {
      for (const auto& input : buses[bi].inserts) {
        // Other insert types were bypassed by the legacy session renderer.
        if (input.type != "spectral_ducker") continue;
        PreparedInsert insert; insert.bus = bi; insert.duck = std::make_unique<SpectralDuckerNode>();
        auto& duck = *insert.duck;
        // Preserve the legacy conversion order and partial-default fallback.
        try {
          if (input.params.contains("mix")) duck.mix = input.params["mix"].get<float>();
          if (input.params.contains("detectorHpfHz")) duck.scHpfHz = input.params["detectorHpfHz"].get<float>();
          if (input.params.contains("applyMode")) duck.applyMode = input.params["applyMode"].get<std::string>() == "dynamicEq" ? SpectralDuckerNode::ApplyMode::DynamicEq : SpectralDuckerNode::ApplyMode::Multiply;
          if (input.params.contains("stereoMode")) duck.stereoMode = input.params["stereoMode"].get<std::string>() == "MidSide" ? SpectralDuckerNode::StereoMode::MidSide : SpectralDuckerNode::StereoMode::LR;
          if (input.params.contains("msSideScale")) duck.msSideScale = input.params["msSideScale"].get<float>();
        } catch (...) {}
        duck.prepareRealtime(sampleRate_, maxFrames_, channels_);
        for (const auto& sidechain : input.sidechains) {
          const size_t ri = rackIndex(sidechain.second);
          if (ri != SIZE_MAX) insert.sidechainRacks.push_back(ri);
        }
        inserts_.push_back(std::move(insert));
      }
    }
    telemetry_.reset(); reportedDrops_ = 0;
    sampleCounter_.store(0, std::memory_order_relaxed);
    lateEvents_.store(0, std::memory_order_relaxed); faults_.store(0, std::memory_order_relaxed);
    lastMetersPrintSec_ = 0.0;
    prepared_ = true;
  }

  void start(const std::vector<Rack>& racks, const std::vector<SessionSpec::BusRef>& buses,
             const std::vector<SessionSpec::RouteRef>& routes, double requestedSampleRate, uint32_t channels) {
    requireStopped();
    if (unit_.valid()) throw std::logic_error("Stop the previous session before configuring another device");
    if (!std::isfinite(requestedSampleRate) || requestedSampleRate <= 0 || (channels != 1 && channels != 2))
      throw std::invalid_argument("Session device requires a finite rate and mono/stereo layout");
    AudioComponentDescription description{};
    description.componentType = kAudioUnitType_Output;
    description.componentSubType = kAudioUnitSubType_DefaultOutput;
    description.componentManufacturer = kAudioUnitManufacturer_Apple;
    const AudioComponent component = AudioComponentFindNext(nullptr, &description);
    if (!component) throw std::runtime_error("Default output component not found");
    OSStatus error = AudioComponentInstanceNew(component, unit_.ptr());
    if (error != noErr) throw std::runtime_error("AudioComponentInstanceNew failed: " + osstatusToString(error));
    AudioStreamBasicDescription format{};
    format.mSampleRate = requestedSampleRate; format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagsNativeFloatPacked; format.mBitsPerChannel = 32;
    format.mChannelsPerFrame = channels; format.mFramesPerPacket = 1;
    format.mBytesPerFrame = 4 * channels; format.mBytesPerPacket = format.mBytesPerFrame;
    error = AudioUnitSetProperty(unit_.get(), kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format, sizeof(format));
    if (error != noErr) throw std::runtime_error("AudioUnitSetProperty(StreamFormat) failed: " + osstatusToString(error));
    AURenderCallbackStruct callback{}; callback.inputProc = &RealtimeSessionRenderer::render; callback.inputProcRefCon = this;
    error = AudioUnitSetProperty(unit_.get(), kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Global, 0, &callback, sizeof(callback));
    if (error != noErr) throw std::runtime_error("AudioUnitSetProperty(SetRenderCallback) failed: " + osstatusToString(error));
    error = AudioUnitInitialize(unit_.get());
    if (error != noErr) throw std::runtime_error("AudioUnitInitialize failed: " + osstatusToString(error));
    UInt32 bytes = sizeof(format);
    error = AudioUnitGetProperty(unit_.get(), kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format, &bytes);
    if (error != noErr || !std::isfinite(format.mSampleRate) || format.mSampleRate <= 0 ||
        format.mChannelsPerFrame != channels)
      throw std::runtime_error("Session device returned an unsupported stream format");
    const double rate = format.mSampleRate;
    UInt32 maximumFrames = 0; bytes = sizeof(maximumFrames);
    error = AudioUnitGetProperty(unit_.get(), kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maximumFrames, &bytes);
    if (error != noErr || maximumFrames == 0)
      throw std::runtime_error("Cannot determine the session device's maximum frames per slice");
    prepareSession(racks, buses, routes, rate, maximumFrames, channels);
  }

  void begin() {
    requireStopped();
    if (!unit_.valid() || !prepared_) throw std::logic_error("Prepare the session device before beginning playback");
    sampleCounter_.store(0, std::memory_order_relaxed);
    const OSStatus error = AudioOutputUnitStart(unit_.get());
    if (error != noErr) throw std::runtime_error("AudioOutputUnitStart failed: " + osstatusToString(error));
    active_ = true;
  }

  void stop() {
    unit_.release(); active_ = false;
    pollDiagnostics();
    if (metricsFile_) { std::fclose(metricsFile_); metricsFile_ = nullptr; }
    metricsEnabled_ = false;
  }
  uint64_t lateEventCount() const noexcept { return lateEvents_.load(std::memory_order_relaxed); }
  uint64_t faultCount() const noexcept { return faults_.load(std::memory_order_relaxed); }
  uint64_t telemetryDropped() const noexcept { return telemetry_.dropped(); }
  double sampleRate() const noexcept { return sampleRate_; }
  SampleTime sampleCounter() const noexcept { return sampleCounter_.load(std::memory_order_relaxed); }
  void resetSampleCounter() { requireStopped(); sampleCounter_.store(0, std::memory_order_relaxed); }

  // Shared device-free callback entry. Bad layouts/capacity silence only the
  // supplied span and consume no events. A DSP fault silences the complete
  // block and requires preparation before retry. The already-started block
  // still advances its absolute sample clock; drained commands are not replayed.
  RenderStatus renderBlock(std::span<float> output, uint32_t frames) noexcept {
    if (rendering_.test_and_set(std::memory_order_acquire)) {
      std::fill(output.begin(), output.end(), 0.0f);
      faults_.fetch_add(1, std::memory_order_relaxed);
      return RenderStatus::NodeFailure; // Never add a second telemetry producer.
    }
    struct Exit { std::atomic_flag& flag; ~Exit() { flag.clear(std::memory_order_release); } } exit{rendering_};
    if (!prepared_) return fault(output, RenderStatus::NotPrepared);
    if (frames > maxFrames_) return fault(output, RenderStatus::CapacityExceeded);
    const size_t samples = static_cast<size_t>(frames) * channels_;
    if (output.size() < samples) return fault(output, RenderStatus::InvalidLayout);
    if (frames == 0) return RenderStatus::Ok; // Do not drain late events into an empty block.
    const SampleTime start = sampleCounter_.load(std::memory_order_relaxed);
    if (frames > std::numeric_limits<SampleTime>::max() - start)
      return fault(output, RenderStatus::InvalidContext);
    const SampleTime cutoff = start + frames;
    const size_t count = cmdQueue_ ? queueDrain_(cmdQueue_, cutoff, eventStorage_) : 0;
    const auto events = std::span<const Command>(eventStorage_).first(count);
    mam::session::SessionEventDispatcher dispatch(events, start);
    if (debug_) { Telemetry record; record.kind = Kind::Block; record.frame = start; record.cutoff = cutoff; record.count = count; telemetry_.push(record); }
    size_t splitCount = 0;
    splitStorage_[splitCount++] = 0; splitStorage_[splitCount++] = frames;
    for (const auto& event : events)
      if (event.sampleTime >= start && event.sampleTime < cutoff)
        splitStorage_[splitCount++] = static_cast<uint32_t>(event.sampleTime - start);
    auto first = splitStorage_.begin(); auto last = first + static_cast<std::ptrdiff_t>(splitCount);
    std::sort(first, last); last = std::unique(first, last);
    splitCount = static_cast<size_t>(last - first);
    try {
      for (size_t segment = 0; segment + 1 < splitCount; ++segment) {
        const uint32_t offset = splitStorage_[segment];
        const uint32_t segmentFrames = splitStorage_[segment + 1] - offset;
        const size_t segmentSamples = static_cast<size_t>(segmentFrames) * channels_;
        const SampleTime segmentStart = start + offset;
        auto out = output.subspan(static_cast<size_t>(offset) * channels_, segmentSamples);
        const auto late = dispatch.dispatchAt(segmentStart,
            [](const Command&) {
              // Compatibility: the legacy "xfader:" parser used an incorrect
              // prefix length and ignored normal manual xfader commands. Keep
              // those commands inert in this sound-neutral preparation pass.
              // LFO xfaders below retain their existing behavior.
            },
            [&](const Command& event) { applyEvent(event, segmentStart); },
            [&](const Command& event) { applyEvent(event, segmentStart); });
        if (late) lateEvents_.fetch_add(late, std::memory_order_relaxed);
        std::fill(out.begin(), out.end(), 0.0f);
        ProcessContext context{sampleRate_, segmentFrames, segmentStart};
        for (size_t ri = 0; ri < racks_.size(); ++ri) {
          auto scratch = std::span<float>(rackScratch_[ri]).first(segmentSamples);
          std::fill(scratch.begin(), scratch.end(), 0.0f);
          if (!activeRacks_[ri] || !racks_[ri].graph) continue;
          const auto result = racks_[ri].graph->processPrepared(context, scratch, channels_);
          if (result != Graph::PreparedProcessStatus::Ok) {
            prepared_ = false;
            sampleCounter_.store(cutoff, std::memory_order_relaxed);
            return fault(output, RenderStatus::NodeFailure);
          }
          if (metersEnabled_) measure(scratch, rackMeters_[ri]);
        }
        for (auto& scratch : busScratch_) std::fill_n(scratch.begin(), segmentSamples, 0.0f);
        std::fill(rackGainMul_.begin(), rackGainMul_.end(), 1.0f);
        for (auto& xfader : xfaders_) {
          if (xfader.lfoEnabled) {
            const double dt = static_cast<double>(segmentFrames) / sampleRate_;
            xfader.phase01 = boundedPhase(xfader.phase01 + xfader.freqHz * dt);
            xfader.xTarget = .5 * (std::sin(2.0 * M_PI * xfader.phase01) + 1.0);
          }
          const double alpha = std::min(1.0, xfader.smoothingMs > 0.0
              ? (static_cast<double>(segmentFrames) / sampleRate_) / (xfader.smoothingMs / 1000.0) : 1.0);
          xfader.x += (xfader.xTarget - xfader.x) * alpha;
          const double x = std::clamp(xfader.x, 0.0, 1.0);
          const double gainA = xfader.lawEqualPower ? std::cos(.5 * M_PI * x) : 1.0 - x;
          const double gainB = xfader.lawEqualPower ? std::sin(.5 * M_PI * x) : x;
          xfader.lastGA = gainA; xfader.lastGB = gainB;
          if (xfader.aIndex < rackGainMul_.size()) rackGainMul_[xfader.aIndex] *= static_cast<float>(gainA);
          if (xfader.bIndex < rackGainMul_.size()) rackGainMul_[xfader.bIndex] *= static_cast<float>(gainB);
        }
        for (const auto& route : compiledRoutes_) {
          const float gain = racks_[route.rack].gain * route.gain * rackGainMul_[route.rack];
          for (size_t i = 0; i < segmentSamples; ++i)
            busScratch_[route.bus][i] += rackScratch_[route.rack][i] * gain;
        }
        for (size_t ri = 0; ri < racks_.size(); ++ri) {
          if (!activeRacks_[ri] || rackHadRoute_[ri]) continue;
          const float gain = rackGainMul_[ri] * racks_[ri].gain;
          for (size_t i = 0; i < segmentSamples; ++i) out[i] += rackScratch_[ri][i] * gain;
        }
        for (auto& insert : inserts_) {
          std::fill_n(sidechainScratch_.begin(), segmentSamples, 0.0f);
          for (const size_t ri : insert.sidechainRacks)
            for (size_t i = 0; i < segmentSamples; ++i) sidechainScratch_[i] += rackScratch_[ri][i];
          // The old renderer created a fresh ducker at every event segment.
          // Preserve that sound; retain allocations, not processing history.
          insert.duck->reset();
          insert.duck->applySidechain(context, busScratch_[insert.bus].data(), sidechainScratch_.data(), channels_);
        }
        for (size_t bi = 0; bi < busScratch_.size(); ++bi) {
          const auto bus = std::span<const float>(busScratch_[bi]).first(segmentSamples);
          if (metersEnabled_) measure(bus, busMeters_[bi]);
          for (size_t i = 0; i < segmentSamples; ++i) out[i] += bus[i];
        }
        publishMeters(cutoff);
      }
    } catch (...) {
      prepared_ = false;
      sampleCounter_.store(cutoff, std::memory_order_relaxed);
      return fault(output, RenderStatus::NodeFailure);
    }
    sampleCounter_.store(cutoff, std::memory_order_relaxed);
    return RenderStatus::Ok;
  }

  // Control thread only; callback records contain no formatted text or JSON.
  // Bound this drain too, so a busy producer cannot starve the control loop.
  void pollDiagnostics() {
    Telemetry record;
    bool wroteMetrics = false;
    for (size_t count = 0; count < telemetry_.capacity() && telemetry_.pop(record); ++count) {
      if (record.kind == Kind::Command) printEvent(record.command, record.frame);
      else if (record.kind == Kind::Block)
        std::fprintf(stderr, "[rt-session] drained=%zu at t=%.3f..%.3f sec\n", record.count,
                     static_cast<double>(record.frame) / sampleRate_, static_cast<double>(record.cutoff) / sampleRate_);
      else if (record.kind == Kind::Fault)
        std::fprintf(stderr, "[rt-session] render fault=%zu at frame=%llu; output silenced\n", record.index,
                     static_cast<unsigned long long>(record.frame));
      else if (record.kind == Kind::RackMeter || record.kind == Kind::BusMeter) {
        const bool rack = record.kind == Kind::RackMeter;
        if (record.index >= (rack ? racks_.size() : busNames_.size())) continue;
        const char* id = rack ? racks_[record.index].id.c_str() : busNames_[record.index].c_str();
        const double peakDb = record.a > 0 ? 20.0 * std::log10(record.a) : -std::numeric_limits<double>::infinity();
        const double rms = record.count ? std::sqrt(record.b / static_cast<double>(record.count)) : 0.0;
        const double rmsDb = rms > 0 ? 20.0 * std::log10(rms) : -std::numeric_limits<double>::infinity();
        std::fprintf(stderr, "Meters\t%s=%s\tpeak_dBFS=%.2f\trms_dBFS=%.2f\n", rack ? "rack" : "bus", id, peakDb, rmsDb);
        if (metricsEnabled_ && metricsFile_ && (rack ? metricsIncludeRacks_ : metricsIncludeBuses_)) {
          const double wall = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
          std::fprintf(metricsFile_, "{\"event\":\"meters\",\"ts_unix\":%.6f,\"t_rel\":%.6f,\"interval_s\":%.3f,\"sr\":%.0f,\"channels\":%u,\"kind\":\"%s\",\"id\":\"%s\",\"peak_dbfs\":%.3f,\"rms_dbfs\":%.3f}\n",
                       wall, static_cast<double>(record.frame) / sampleRate_, metersIntervalSec_, sampleRate_, channels_, rack ? "rack" : "bus", id, peakDb, rmsDb);
          wroteMetrics = true;
        }
      } else if (record.kind == Kind::Xfader && record.index < xfaders_.size() && metricsEnabled_ && metricsFile_) {
        const double wall = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        std::fprintf(metricsFile_, "{\"event\":\"xfader\",\"ts_unix\":%.6f,\"t_rel\":%.6f,\"id\":\"%s\",\"x\":%.4f,\"gainA\":%.4f,\"gainB\":%.4f}\n",
                     wall, static_cast<double>(record.frame) / sampleRate_, xfaders_[record.index].id.c_str(), record.a, record.b, record.c);
        wroteMetrics = true;
      }
    }
    if (wroteMetrics && metricsFile_) std::fflush(metricsFile_);
    const auto dropped = telemetry_.dropped();
    if (dropped != reportedDrops_) {
      std::fprintf(stderr, "[rt-session] diagnostic records dropped=%llu total\n", static_cast<unsigned long long>(dropped));
      reportedDrops_ = dropped;
    }
  }

private:
  using Kind = mam::realtime::TelemetryKind;
  using Telemetry = mam::realtime::TelemetryRecord;
  struct Meter { double sumSq = 0.0; float peak = 0.0f; uint64_t frames = 0; };
  struct Route { size_t rack; size_t bus; float gain; };
  struct NodeTarget { std::string id; Node* node; };
  struct PreparedInsert { size_t bus = 0; std::unique_ptr<SpectralDuckerNode> duck; std::vector<size_t> sidechainRacks; };
  struct XfaderState {
    std::string id, aRackId, bRackId; size_t aIndex = SIZE_MAX; size_t bIndex = SIZE_MAX;
    bool lawEqualPower = true; double smoothingMs = 10; bool lfoEnabled = false;
    double freqHz = .25; double phase01 = 0; double x = .5; double xTarget = .5;
    double initialPhase01 = 0; double initialX = .5;
    double lastGA = 1; double lastGB = 1;
  };
  // Finite out-of-range phases wrap in bounded time. For ordinary positive
  // increments (< one cycle), retain the legacy subtraction and arithmetic.
  // Nonfinite settings and overflowing prepared increments are rejected above.
  static double boundedPhase(double phase) noexcept {
    if (phase >= 1.0 && phase < 2.0) return phase - 1.0;
    if (phase >= 2.0 || phase < 0.0) {
      phase = std::fmod(phase, 1.0);
      if (phase < 0.0) phase += 1.0;
    }
    return phase;
  }
  void requireStopped() const {
    if (active_) throw std::logic_error("Session configuration requires stopped playback");
  }
  RenderStatus fault(std::span<float> output, RenderStatus status) noexcept {
    std::fill(output.begin(), output.end(), 0.0f);
    faults_.fetch_add(1, std::memory_order_relaxed);
    Telemetry record; record.kind = Kind::Fault; record.index = static_cast<size_t>(status);
    record.frame = sampleCounter_.load(std::memory_order_relaxed); telemetry_.push(record);
    return status;
  }
  static void measure(std::span<const float> input, Meter& meter) noexcept {
    for (const float value : input) {
      meter.peak = std::max(meter.peak, std::fabs(value));
      meter.sumSq += static_cast<double>(value) * static_cast<double>(value);
    }
    meter.frames += input.size();
  }
  void applyEvent(const Command& event, SampleTime actualFrame) {
    if (event.nodeId) {
      const std::string_view id(event.nodeId);
      auto target = std::lower_bound(nodeTargets_.begin(), nodeTargets_.end(), id,
          [](const NodeTarget& entry, std::string_view key) { return std::string_view(entry.id) < key; });
      for (; target != nodeTargets_.end() && target->id == id; ++target) target->node->handleEvent(event);
    }
    if (printTriggers_) { Telemetry record; record.kind = Kind::Command; record.command = event; record.frame = actualFrame; telemetry_.push(record); }
  }
  void publishMeters(SampleTime cutoff) noexcept {
    if (!metersEnabled_) return;
    const double now = static_cast<double>(cutoff) / sampleRate_;
    if (now - lastMetersPrintSec_ < metersIntervalSec_) return;
    for (size_t i = 0; i < rackMeters_.size(); ++i) {
      const auto& meter = rackMeters_[i]; if (!meter.frames) continue;
      Telemetry record; record.kind = Kind::RackMeter; record.index = i;
      record.frame = cutoff; record.count = meter.frames; record.a = meter.peak; record.b = meter.sumSq;
      telemetry_.push(record);
    }
    for (size_t i = 0; i < busMeters_.size(); ++i) {
      const auto& meter = busMeters_[i]; if (!meter.frames) continue;
      Telemetry record; record.kind = Kind::BusMeter; record.index = i;
      record.frame = cutoff; record.count = meter.frames; record.a = meter.peak; record.b = meter.sumSq;
      telemetry_.push(record);
    }
    if (metricsEnabled_) for (size_t i = 0; i < xfaders_.size(); ++i) {
      Telemetry record; record.kind = Kind::Xfader; record.index = i; record.frame = cutoff;
      record.a = xfaders_[i].x; record.b = xfaders_[i].lastGA; record.c = xfaders_[i].lastGB;
      telemetry_.push(record);
    }
    std::fill(rackMeters_.begin(), rackMeters_.end(), Meter{});
    std::fill(busMeters_.begin(), busMeters_.end(), Meter{});
    lastMetersPrintSec_ = now;
  }
  static OSStatus render(void* reference, AudioUnitRenderActionFlags*, const AudioTimeStamp*,
                         UInt32, UInt32 frames, AudioBufferList* buffers) noexcept {
    auto* self = static_cast<RealtimeSessionRenderer*>(reference);
    if (!buffers || buffers->mNumberBuffers != 1 || !buffers->mBuffers[0].mData ||
        buffers->mBuffers[0].mNumberChannels != self->channels_) {
      if (buffers) for (UInt32 i = 0; i < buffers->mNumberBuffers; ++i)
        if (buffers->mBuffers[i].mData) std::memset(buffers->mBuffers[i].mData, 0, buffers->mBuffers[i].mDataByteSize);
      self->faults_.fetch_add(1, std::memory_order_relaxed);
      return noErr;
    }
    auto& buffer = buffers->mBuffers[0];
    self->renderBlock({static_cast<float*>(buffer.mData), buffer.mDataByteSize / sizeof(float)}, frames);
    return noErr; // Faults are explicit telemetry and silence, never callback I/O.
  }
  void printEvent(const Command& c, SampleTime segAbsStart) const {
    const double tSec = static_cast<double>(segAbsStart) / sampleRate_;
    const char* src = (c.source == 1 ? "SESS" : "RACK");
    const char* tag = (c.type == CommandType::Trigger) ? "TRIGGER" : (c.type == CommandType::SetParam ? "SET" : "RAMP");
    const char* node = c.nodeId ? c.nodeId : "";
    // For triggers, omit pid entirely
    if (c.type == CommandType::Trigger) {
      std::fprintf(stderr, "%.6f\t%s\t%s\tnode=%s\n", tSec, tag, src, node);
      return;
    }
    // Prefer param name when available for known node types, and special-case xfader:x
    const char* pname = nullptr;
    if (node && *node) {
      // xfader special case: nodeId format "xfader:<id>:x"
      if (std::strncmp(node, "xfader:", 7) == 0) {
        const char* rest = node + 7; const char* colon = std::strchr(rest, ':');
        if (colon && std::strcmp(colon + 1, "x") == 0) pname = "x";
      }
      if (!pname && c.paramNameStr && *c.paramNameStr) {
        pname = c.paramNameStr; // fallback to carried name
      }
      if (!pname && c.paramId != 0) {
        auto it = nodeTypeById_.find(std::string(node));
        if (it != nodeTypeById_.end()) {
          const std::string& t = it->second;
          auto nameById = [&](const ParamMap& m, uint16_t id) -> const char* {
            for (size_t i = 0; i < m.count; ++i) if (m.defs[i].id == id) return m.defs[i].name;
            return nullptr;
          };
          if (t == std::string("kick")) pname = nameById(kKickParamMap, c.paramId);
          else if (t == std::string("clap")) pname = nameById(kClapParamMap, c.paramId);
          else if (t == std::string("tb303_ext")) pname = nameById(kTb303ParamMap, c.paramId);
          else if (t == std::string("mam_chip")) pname = nameById(kMamChipParamMap, c.paramId);
        }
      }
    }
    // Print with extra tab spacing after node and without labels
    if (pname) std::fprintf(stderr, "%.6f\t%s\t%s\tnode=%s\t\t%s\t%.3f\n", tSec, tag, src, node, pname, static_cast<double>(c.value));
    else std::fprintf(stderr, "%.6f\t%s\t%s\tnode=%s\t\t%.3f\n", tSec, tag, src, node, static_cast<double>(c.value));
  }

  AudioUnitHandle unit_{};
  std::vector<Rack> racks_;
  std::vector<std::string> busNames_;
  std::vector<Route> compiledRoutes_;
  std::vector<NodeTarget> nodeTargets_;
  std::vector<PreparedInsert> inserts_;
  std::vector<XfaderState> xfaders_;
  std::unordered_map<std::string, std::string> nodeTypeById_;
  std::vector<std::vector<float>> rackScratch_, busScratch_;
  std::vector<float> sidechainScratch_, rackGainMul_;
  std::vector<bool> activeRacks_, rackHadRoute_;
  std::vector<Meter> rackMeters_, busMeters_;
  std::vector<Command> eventStorage_;
  std::vector<uint32_t> splitStorage_;
  uint32_t channels_ = 2, maxFrames_ = 0;
  double sampleRate_ = 48000;
  std::atomic<SampleTime> sampleCounter_{0};
  std::atomic<uint64_t> lateEvents_{0}, faults_{0};
  std::atomic_flag rendering_ = ATOMIC_FLAG_INIT;
  void* cmdQueue_ = nullptr;
  using DrainFn = size_t (*)(void*, SampleTime, std::span<Command>) noexcept;
  DrainFn queueDrain_ = nullptr;
  size_t eventCapacity_ = 0;
  bool prepared_ = false, active_ = false;
  bool printTriggers_ = false, debug_ = false, metersEnabled_ = false;
  double metersIntervalSec_ = 1.0, lastMetersPrintSec_ = 0.0;
  bool metricsEnabled_ = false, metricsIncludeRacks_ = true, metricsIncludeBuses_ = true;
  FILE* metricsFile_ = nullptr;
  mam::realtime::TelemetryQueue<> telemetry_;
  uint64_t reportedDrops_ = 0;
};
