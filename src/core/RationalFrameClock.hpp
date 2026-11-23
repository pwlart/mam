#pragma once

#include <cstdint>

namespace mam::timing {

// Preparation-side conversion from absolute ticks to uint64 frame positions.
// A tick's musical meaning is the caller's responsibility. This clock accepts
// an exact positive frames-per-tick ratio, not floating-point BPM or seconds.
// Production transport paths do not use it yet; see docs/TIMING.md.
class RationalFrameClock {
public:
  // Both values must be positive; otherwise throws std::invalid_argument.
  RationalFrameClock(uint64_t numeratorFrames, uint64_t denominatorTicks);

  // Round absoluteTick * numerator / denominator to nearest, with ties upward.
  // Throws std::overflow_error only when the rounded position exceeds uint64.
  // The multiplication itself need not fit uint64. No accumulated step rounding.
  [[nodiscard]] uint64_t frameAt(uint64_t absoluteTick) const;

  // Difference of rounded absolute endpoints, preserving the starting phase.
  // Rejects endTick < beginTick and unrepresentable absolute endpoints.
  [[nodiscard]] uint64_t framesBetween(uint64_t beginTick, uint64_t endTick) const;

private:
  uint64_t numerator_;
  uint64_t denominator_;
};

} // namespace mam::timing
