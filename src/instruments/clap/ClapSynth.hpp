#pragma once

#include <cstdint>
#include "../../core/Random.hpp"

struct ClapParams {
  float ampDecayMs = 180.0f;
  float gain = 0.9f;
  float bpm = 0.0f;   // if > 0, loop at BPM
  bool loop = false;
};

class ClapSynth {
public:
  explicit ClapSynth(const ClapParams& params, double sampleRate);
  void setSampleRate(double sr);
  double sampleRate() const;
  void setRandomSeed(uint32_t seed) noexcept;
  void reset();
  void trigger();
  void trigger(float velocity);
  float process();
  const ClapParams& params() const;
  ClapParams& params();

private:
  inline float nextNoise();

  ClapParams params_{};
  double sampleRate_ = 48000.0;
  double tSec_ = 0.0;
  uint64_t framesUntilNextTrigger_ = 0;
  bool active_ = false;
  bool triggeredOnce_ = false;
  mam::random::NoiseRandom noise_;
  float velocity_ = 1.0f; // 0..1, applied to output
};
