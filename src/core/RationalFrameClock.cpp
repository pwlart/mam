#include "RationalFrameClock.hpp"

#include <limits>
#include <numeric>
#include <stdexcept>

namespace mam::timing {
namespace {

uint64_t checkedAdd(uint64_t left, uint64_t right) {
  if (right > std::numeric_limits<uint64_t>::max() - left) {
    throw std::overflow_error("Rational frame position exceeds uint64");
  }
  return left + right;
}

} // namespace

RationalFrameClock::RationalFrameClock(uint64_t numeratorFrames, uint64_t denominatorTicks)
    : numerator_(numeratorFrames), denominator_(denominatorTicks) {
  if (numerator_ == 0 || denominator_ == 0) {
    throw std::invalid_argument("A frame clock requires a positive ratio");
  }
  const auto divisor = std::gcd(numerator_, denominator_);
  numerator_ /= divisor;
  denominator_ /= divisor;
}

uint64_t RationalFrameClock::frameAt(uint64_t absoluteTick) const {
  const uint64_t whole = numerator_ / denominator_;
  const uint64_t part = numerator_ % denominator_;
  if (whole != 0 && absoluteTick > std::numeric_limits<uint64_t>::max() / whole) {
    throw std::overflow_error("Rational frame position exceeds uint64");
  }
  const uint64_t wholeFrames = absoluteTick * whole;
  if (part == 0) return wholeFrames;

  // Binary long multiplication/division of absoluteTick * part / denominator.
  // After each bit, quotient/remainder describe the consumed tick prefix.
  // Since part < denominator, the quotient never exceeds that prefix: doubling
  // it is safe. Modular additions use subtraction at the wrap to avoid forming
  // overflowing 2*remainder or remainder+part intermediates. All 64 input bits
  // are handled with standard C++20 integers, without compiler-specific types.
  uint64_t quotient = 0;
  uint64_t remainder = 0;
  for (int bit = 63; bit >= 0; --bit) {
    quotient *= 2;
    if (remainder >= denominator_ - remainder) {
      remainder -= denominator_ - remainder;
      ++quotient;
    } else {
      remainder += remainder;
    }
    if (((absoluteTick >> bit) & 1u) != 0) {
      if (remainder >= denominator_ - part) {
        remainder -= denominator_ - part;
        ++quotient;
      } else {
        remainder += part;
      }
    }
  }

  uint64_t frame = checkedAdd(wholeFrames, quotient);
  if (remainder >= denominator_ - remainder) frame = checkedAdd(frame, 1);
  return frame;
}

uint64_t RationalFrameClock::framesBetween(uint64_t beginTick, uint64_t endTick) const {
  if (endTick < beginTick) {
    throw std::invalid_argument("Frame interval end precedes its beginning");
  }
  return frameAt(endTick) - frameAt(beginTick);
}

} // namespace mam::timing
