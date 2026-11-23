#pragma once

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudioTypes.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include "../core/Graph.hpp"
#include "../core/ScopedAudioUnit.hpp"
#include "../core/OsStatusUtils.hpp"
#include "../core/Command.hpp"
#include "../session/SessionFeeding.hpp"
#include "RealtimeTelemetry.hpp"

// Configuration/preparation and diagnostics polling belong to the control
// thread. renderBlock is the same device-free path called by CoreAudio; only
// that single consumer may mutate the prepared graph while it is running.
// Graph, command queue and every borrowed Command string must outlive stop()
// and the final pollDiagnostics(); device-free callers also stop renderBlock
// invocations before changing configuration, preparing or releasing owners.
class RealtimeGraphRenderer {
public:
  enum class RenderStatus { Ok, NotPrepared, InvalidBuffer, CapacityExceeded, GraphFailure, TimeOverflow };

  RealtimeGraphRenderer() = default;
  ~RealtimeGraphRenderer() { stop(); }

  void setCommandQueue(SpscCommandQueue<2048>* queue) { requireStopped(); cmdQueue_ = queue; }
  void setCmdQueueDebug(bool enabled) { requireStopped(); queueDiag_ = enabled; }
  void setDiagnostics(bool printTriggers, double bpmForBeats, uint32_t resolutionStepsPerBar) {
    requireStopped();
    printTriggers_ = printTriggers;
    diagBpm_ = bpmForBeats;
    diagResolution_ = resolutionStepsPerBar == 0 ? 16u : resolutionStepsPerBar;
  }
  void setDiagLoop(uint64_t loopFrames) { requireStopped(); diagLoopFrames_ = loopFrames; }
  void setTransportEmitEnabled(bool enabled) { requireStopped(); transportEmitEnabled_ = enabled; }

  void prepareGraph(Graph& graph, double sampleRate, uint32_t maxFrames, uint32_t channels) {
    requireStopped();
    prepared_ = false;
    if (transportEmitEnabled_)
      throw std::invalid_argument("Prepared realtime graphs require queued transport events");
    graph.prepareRealtime(sampleRate, maxFrames, channels);
    graph.reset();
    graph_ = &graph;
    sampleRate_ = sampleRate;
    channels_ = channels;
    maxFrames_ = maxFrames;
    sampleCounter_.store(0, std::memory_order_relaxed);
    lateEvents_.store(0, std::memory_order_relaxed);
    faults_.store(0, std::memory_order_relaxed);
    telemetry_.reset();
    reportedDropped_ = 0;
    prepared_ = true;
  }

  // Device setup does not activate the consumer. The caller primes its bounded
  // event queue at the actual device rate, then calls begin().
  void start(Graph& graph, double requestedSampleRate, uint32_t channels) {
    if (unit_.valid()) throw std::logic_error("Realtime graph device is already prepared");
    if (channels != 1 && channels != 2) throw std::invalid_argument("Realtime graph supports mono or stereo");
    AudioComponentDescription desc{};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_DefaultOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    AudioComponent component = AudioComponentFindNext(nullptr, &desc);
    if (!component) throw std::runtime_error("Default output component not found");
    OSStatus error = AudioComponentInstanceNew(component, unit_.ptr());
    checkStatus(error, "AudioComponentInstanceNew");

    AudioStreamBasicDescription format{};
    format.mSampleRate = requestedSampleRate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
    format.mBitsPerChannel = 32;
    format.mChannelsPerFrame = channels;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = sizeof(float) * channels;
    format.mBytesPerPacket = format.mBytesPerFrame;
    error = AudioUnitSetProperty(unit_.get(), kAudioUnitProperty_StreamFormat,
        kAudioUnitScope_Input, 0, &format, sizeof(format));
    checkStatus(error, "AudioUnitSetProperty(StreamFormat)");
    AURenderCallbackStruct callback{};
    callback.inputProc = &RealtimeGraphRenderer::render;
    callback.inputProcRefCon = this;
    error = AudioUnitSetProperty(unit_.get(), kAudioUnitProperty_SetRenderCallback,
        kAudioUnitScope_Global, 0, &callback, sizeof(callback));
    checkStatus(error, "AudioUnitSetProperty(SetRenderCallback)");
    checkStatus(AudioUnitInitialize(unit_.get()), "AudioUnitInitialize");

    UInt32 size = sizeof(format);
    checkStatus(AudioUnitGetProperty(unit_.get(), kAudioUnitProperty_StreamFormat,
        kAudioUnitScope_Input, 0, &format, &size), "AudioUnitGetProperty(StreamFormat)");
    UInt32 maximumFrames = 0;
    size = sizeof(maximumFrames);
    checkStatus(AudioUnitGetProperty(unit_.get(), kAudioUnitProperty_MaximumFramesPerSlice,
        kAudioUnitScope_Global, 0, &maximumFrames, &size), "AudioUnitGetProperty(MaximumFramesPerSlice)");
    if (format.mChannelsPerFrame != channels)
      throw std::runtime_error("AudioUnit changed the requested channel layout");
    prepareGraph(graph, format.mSampleRate, maximumFrames, channels);
  }

  void begin() {
    if (!unit_.valid() || !prepared_) throw std::logic_error("Realtime graph is not prepared");
    checkStatus(AudioOutputUnitStart(unit_.get()), "AudioOutputUnitStart");
    running_ = true;
  }
  void stop() { unit_.release(); running_ = false; }

  // All storage is fixed during preparation. Unsupported blocks return silence
  // and a numeric fault; event/graph failures require preparation before retry.
  RenderStatus renderBlock(std::span<float> output, uint32_t frames) noexcept {
    const SampleTime blockStart = sampleCounter_.load(std::memory_order_relaxed);
    if (frames > std::numeric_limits<SampleTime>::max() - blockStart)
      return fail(output, RenderStatus::TimeOverflow, blockStart);
    const SampleTime cutoff = blockStart + frames;
    if (!prepared_ || !graph_) return fail(output, RenderStatus::NotPrepared, blockStart);
    if (frames > maxFrames_) return fail(output, RenderStatus::CapacityExceeded, blockStart);
    if (output.size() < static_cast<size_t>(frames) * channels_)
      return fail(output, RenderStatus::InvalidBuffer, blockStart);
    if (frames == 0) return RenderStatus::Ok;

    using mam::realtime::TelemetryKind;
    using mam::realtime::TelemetryRecord;
    if (printTriggers_ && diagLoopFrames_ > 0) {
      const auto remainder = blockStart % diagLoopFrames_;
      const auto distance = remainder == 0 ? 0 : diagLoopFrames_ - remainder;
      if (distance < frames) {
        TelemetryRecord record;
        record.kind = TelemetryKind::Loop;
        record.frame = blockStart + distance;
        record.index = static_cast<size_t>(record.frame / diagLoopFrames_);
        telemetry_.push(record);
      }
    }
    const size_t count = cmdQueue_ ? cmdQueue_->drainUpToBounded(cutoff, drained_) : 0;
    if (queueDiag_) {
      TelemetryRecord record;
      record.kind = TelemetryKind::Block;
      record.frame = blockStart;
      record.cutoff = cutoff;
      record.count = count;
      telemetry_.push(record);
    }
    size_t splitCount = 1;
    splits_[0] = 0;
    for (size_t i = 0; i < count; ++i) {
      if (drained_[i].sampleTime > blockStart) {
        const auto offset = static_cast<uint32_t>(drained_[i].sampleTime - blockStart);
        if (offset != splits_[splitCount - 1]) splits_[splitCount++] = offset;
      }
    }
    splits_[splitCount++] = frames;
    mam::session::SessionEventDispatcher dispatcher(std::span<const Command>(drained_.data(), count), blockStart);
    bool eventFailed = false;
    const auto deliver = [&](const Command& command) noexcept {
      if (printTriggers_) {
        TelemetryRecord record;
        record.kind = TelemetryKind::Command;
        record.command = command;
        telemetry_.push(record);
      }
      if (!graph_->handlePreparedEvent(command)) eventFailed = true;
    };
    for (size_t split = 0; split + 1 < splitCount; ++split) {
      const auto segmentStart = splits_[split];
      const auto segmentFrames = splits_[split + 1] - segmentStart;
      const auto late = dispatcher.dispatchAt(blockStart + segmentStart,
          [](const Command&) noexcept {}, deliver, deliver);
      lateEvents_.fetch_add(late, std::memory_order_relaxed);
      if (eventFailed) {
        prepared_ = false;
        sampleCounter_.store(cutoff, std::memory_order_relaxed);
        return fail(output, RenderStatus::GraphFailure, blockStart);
      }
      ProcessContext context{};
      context.sampleRate = sampleRate_;
      context.frames = segmentFrames;
      context.blockStart = blockStart + segmentStart;
      const auto segment = output.subspan(static_cast<size_t>(segmentStart) * channels_,
          static_cast<size_t>(segmentFrames) * channels_);
      const auto status = graph_->processPrepared(context, segment, channels_);
      if (status != Graph::PreparedProcessStatus::Ok) {
        prepared_ = false;
        sampleCounter_.store(cutoff, std::memory_order_relaxed);
        return fail(output, RenderStatus::GraphFailure, blockStart);
      }
    }
    sampleCounter_.store(cutoff, std::memory_order_relaxed);
    return RenderStatus::Ok;
  }

  // Only the control thread formats diagnostics. It may drop telemetry when
  // stalled; that does not drop or coalesce commands delivered to the graph.
  void pollDiagnostics() {
    mam::realtime::TelemetryRecord record;
    for (size_t count = 0; count < telemetry_.capacity() && telemetry_.pop(record); ++count) {
      switch (record.kind) {
        case mam::realtime::TelemetryKind::Command:
          printEvent(record.command.type == CommandType::Trigger ? "TRIGGER" :
              (record.command.type == CommandType::SetParam ? "SET" : "RAMP"), record.command);
          break;
        case mam::realtime::TelemetryKind::Loop:
          std::fprintf(stderr, "Loop %llu\n", static_cast<unsigned long long>(record.index));
          break;
        case mam::realtime::TelemetryKind::Block:
          std::fprintf(stderr, "[rt] drained %zu events up to %llu (block %llu..%llu)\n", record.count,
              static_cast<unsigned long long>(record.cutoff), static_cast<unsigned long long>(record.frame),
              static_cast<unsigned long long>(record.cutoff));
          break;
        case mam::realtime::TelemetryKind::Fault:
          std::fprintf(stderr, "[rt-graph] render fault=%zu at frame=%llu\n", record.index,
              static_cast<unsigned long long>(record.frame));
          break;
        default: break;
      }
    }
    const auto dropped = telemetry_.dropped();
    if (dropped != reportedDropped_) {
      std::fprintf(stderr, "[rt-graph] diagnostic records dropped: %llu\n", static_cast<unsigned long long>(dropped));
      reportedDropped_ = dropped;
    }
  }

  double sampleRate() const noexcept { return sampleRate_; }
  SampleTime sampleCounter() const noexcept { return sampleCounter_.load(std::memory_order_relaxed); }
  uint64_t lateEventCount() const noexcept { return lateEvents_.load(std::memory_order_relaxed); }
  uint64_t faultCount() const noexcept { return faults_.load(std::memory_order_relaxed); }
  uint64_t droppedTelemetryCount() const noexcept { return telemetry_.dropped(); }

private:
  void requireStopped() const {
    if (running_) throw std::logic_error("Stop realtime graph before changing configuration");
  }
  static void checkStatus(OSStatus status, const char* operation) {
    if (status != noErr) throw std::runtime_error(std::string(operation) + " failed: " + osstatusToString(status));
  }
  RenderStatus fail(std::span<float> output, RenderStatus status, SampleTime frame) noexcept {
    std::fill(output.begin(), output.end(), 0.0f);
    faults_.fetch_add(1, std::memory_order_relaxed);
    mam::realtime::TelemetryRecord record;
    record.kind = mam::realtime::TelemetryKind::Fault;
    record.index = static_cast<size_t>(status);
    record.frame = frame;
    telemetry_.push(record);
    return status;
  }
  static OSStatus render(void* context, AudioUnitRenderActionFlags* flags, const AudioTimeStamp*,
                         UInt32, UInt32 frames, AudioBufferList* buffers) noexcept {
    auto* self = static_cast<RealtimeGraphRenderer*>(context);
    if (!buffers || buffers->mNumberBuffers != 1 || !buffers->mBuffers[0].mData ||
        buffers->mBuffers[0].mNumberChannels != self->channels_) {
      if (buffers) {
        for (UInt32 i = 0; i < buffers->mNumberBuffers; ++i) {
          auto& buffer = buffers->mBuffers[i];
          if (buffer.mData) std::memset(buffer.mData, 0, buffer.mDataByteSize);
        }
      }
      self->fail({}, RenderStatus::InvalidBuffer, self->sampleCounter());
      if (flags) *flags |= kAudioUnitRenderAction_OutputIsSilence;
      return noErr;
    }
    auto& buffer = buffers->mBuffers[0];
    const auto result = self->renderBlock(std::span<float>(static_cast<float*>(buffer.mData),
        buffer.mDataByteSize / sizeof(float)), frames);
    if (result != RenderStatus::Ok && flags) *flags |= kAudioUnitRenderAction_OutputIsSilence;
    return noErr;
  }

  AudioUnitHandle unit_{};
  Graph* graph_ = nullptr;
  uint32_t channels_ = 2;
  uint32_t maxFrames_ = 0;
  double sampleRate_ = 48000.0;
  bool prepared_ = false;
  bool running_ = false;
  std::atomic<SampleTime> sampleCounter_{0};
  std::atomic<uint64_t> lateEvents_{0};
  std::atomic<uint64_t> faults_{0};
  SpscCommandQueue<2048>* cmdQueue_ = nullptr;
  std::array<Command, 2047> drained_{};
  std::array<uint32_t, 2049> splits_{};
  mam::realtime::TelemetryQueue<> telemetry_;
  uint64_t reportedDropped_ = 0;
  bool printTriggers_ = false;
  double diagBpm_ = 120.0;
  uint32_t diagResolution_ = 16;
  bool transportEmitEnabled_ = false;
  uint64_t diagLoopFrames_ = 0;
  bool queueDiag_ = false;

  void printEvent(const char* tag, const Command& c) const {
    const uint64_t st = c.sampleTime;
    const uint64_t within = (diagLoopFrames_ > 0) ? (st % diagLoopFrames_) : st;
    // Compute integer bar/step using samples to avoid FP drift
    const double framesPerBarD = (diagBpm_ > 0.0) ? ((60.0 * 4.0) * sampleRate_ / diagBpm_) : (sampleRate_ * 2.0);
    const uint64_t framesPerBar = static_cast<uint64_t>(framesPerBarD + 0.5);
    const uint64_t barIdx = (framesPerBar > 0) ? (within / framesPerBar) : 0ull;
    const uint64_t withinBar = (framesPerBar > 0) ? (within % framesPerBar) : within;
    const uint64_t stepIdx = (framesPerBar > 0 && diagResolution_ > 0)
      ? ((withinBar * static_cast<uint64_t>(diagResolution_) + framesPerBar / 2) / framesPerBar)
      : 0ull;
    const uint64_t stepClamped = (stepIdx >= diagResolution_) ? (diagResolution_ - 1) : stepIdx;
    const double tSec = static_cast<double>(within) / sampleRate_;
    // New tab-delimited format: time_s \t CMD \t bar \t step \t node \t pid \t val
    std::fprintf(stderr, "%.6f\t%s\tbar=%llu\tstep=%llu\tnode=%s\tpid=%u\tval=%.3f\n",
                 tSec, tag,
                 static_cast<unsigned long long>(barIdx + 1ull), static_cast<unsigned long long>(stepClamped + 1ull),
                 c.nodeId ? c.nodeId : "", c.paramId, static_cast<double>(c.value));
  }
};
