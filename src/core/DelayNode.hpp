#pragma once

#include "Node.hpp"
#include <vector>
#include <algorithm>
#include <limits>
#include <stdexcept>

// Simple interleaved feedback delay as an insert effect
class DelayNode final : public Node {
public:
  DelayNode() = default;
  const char* name() const override { return "DelayNode"; }

  void prepare(double sampleRate, uint32_t /*maxBlock*/) override {
    realtimePrepared_ = false;
    sampleRate_ = sampleRate;
    setDelayMs(delayMs); // recompute samples
    // allocate minimum buffer
    const size_t need = static_cast<size_t>(std::max<uint32_t>(1, delaySamples_)) * 2u;
    if (delay_.size() < need) delay_.assign(need, 0.0f);
    writeIndex_ = 0;
  }

  // Allocate for the actual channel layout before realtime processing. No
  // audio is processed to warm the delay, and the legacy prepare path stays
  // available for offline callers that intentionally change their layout.
  void prepareRealtime(double sampleRate, uint32_t maxFrames, uint32_t channels) {
    const float frames = delayMs * static_cast<float>(sampleRate) * 0.001f + 0.5f;
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0 || maxFrames == 0 ||
        (channels != 1 && channels != 2) || !std::isfinite(delayMs) || delayMs < 0.0f ||
        !std::isfinite(frames) || static_cast<double>(frames) > std::numeric_limits<uint32_t>::max())
      throw std::invalid_argument("Invalid realtime delay preparation");
    prepare(sampleRate, maxFrames);
    ensureBuffer(channels);
    reset();
    realtimeChannels_ = channels;
    realtimeMaxFrames_ = maxFrames;
    realtimeDelaySamples_ = delaySamples_;
    realtimePrepared_ = true;
  }

  void reset() override {
    std::fill(delay_.begin(), delay_.end(), 0.0f);
    writeIndex_ = 0;
  }

  void process(ProcessContext /*ctx*/, float* interleavedOut, uint32_t /*channels*/) override {
    // as a generator this does nothing
    (void)interleavedOut;
  }

  // Insert processing: in-place wet/dry mix
  virtual void processInPlace(ProcessContext ctx, float* interleaved, uint32_t channels) override {
    if (channels == 0 || delaySamples_ == 0) return;
    if (realtimePrepared_) {
      if (channels != realtimeChannels_ || ctx.frames > realtimeMaxFrames_ ||
          delaySamples_ != realtimeDelaySamples_ || ctx.sampleRate != sampleRate_) {
        std::fill_n(interleaved, static_cast<size_t>(ctx.frames) * channels, 0.0f);
        return;
      }
    } else {
      ensureBuffer(channels);
    }
    const uint32_t frames = ctx.frames;
    const float fb = std::clamp(feedback, 0.0f, 0.95f);
    const float mixAmt = std::clamp(mix, 0.0f, 1.0f);
    const float dryAmt = 1.0f - mixAmt;
    for (uint32_t n = 0; n < frames; ++n) {
      for (uint32_t ch = 0; ch < channels; ++ch) {
        const size_t delayLen = delay_.size() / channels;
        const size_t readIndex = (writeIndex_ + delayLen - delaySamples_) % delayLen;
        float* line = &delay_[delayLen * ch];
        const float delayed = line[readIndex];
        float& sample = interleaved[n * channels + ch];
        const float input = sample;
        // wet/dry mix
        sample = input * dryAmt + delayed * mixAmt;
        // write new value into delay line with feedback
        line[writeIndex_] = input + delayed * fb;
      }
      writeIndex_ = (writeIndex_ + 1) % (delay_.size() / channels);
    }
  }

  // Parameters
  float delayMs = 350.0f;
  float feedback = 0.35f; // 0..0.95
  float mix = 0.25f;      // 0..1

  void setDelayMs(float ms) {
    delayMs = std::max(0.0f, ms);
    delaySamples_ = static_cast<uint32_t>(delayMs * static_cast<float>(sampleRate_) * 0.001f + 0.5f);
    if (delaySamples_ == 0) delaySamples_ = 1;
  }

private:
  double sampleRate_ = 48000.0;
  uint32_t delaySamples_ = 1;
  std::vector<float> delay_{}; // interleaved per channel sections
  size_t writeIndex_ = 0;      // per channel shared index
  bool realtimePrepared_ = false;
  uint32_t realtimeChannels_ = 0;
  uint32_t realtimeMaxFrames_ = 0;
  uint32_t realtimeDelaySamples_ = 0;

  void ensureBuffer(uint32_t channels) {
    const size_t need = static_cast<size_t>(std::max<uint32_t>(1, delaySamples_)) * channels;
    if (delay_.size() != need) delay_.assign(need, 0.0f);
    if (writeIndex_ >= (delay_.size() / channels)) writeIndex_ = 0;
  }
  uint32_t latencySamples() const override { return delaySamples_; }
};
