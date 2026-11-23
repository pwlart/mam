#include "core/RationalFrameClock.hpp"
#include <doctest/doctest.h>
#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace {
using mam::timing::RationalFrameClock;
constexpr auto maximum = std::numeric_limits<uint64_t>::max();
struct Answer { uint64_t tick; uint64_t frame; };
}

TEST_CASE("rational frame clock rejects zero ratios and reversed intervals") {
  CHECK_THROWS_AS(RationalFrameClock(0, 1), std::invalid_argument);
  CHECK_THROWS_AS(RationalFrameClock(1, 0), std::invalid_argument);
  CHECK_THROWS_AS(RationalFrameClock(0, 0), std::invalid_argument);
  const RationalFrameClock clock(1, 1);
  CHECK_THROWS_AS((void)clock.framesBetween(1, 0), std::invalid_argument);
  CHECK(clock.framesBetween(maximum, maximum) == 0);
}

TEST_CASE("rational frame clock keeps exact 48 kHz sixteenth note positions") {
  // 48,000 frames/s * 60 s/min / (120 quarter notes/min * 4 ticks/quarter).
  const RationalFrameClock clock(2880000, 480);
  constexpr std::array<Answer, 7> answers{{
      {0, 0}, {1, 6000}, {4, 24000}, {16, 96000}, {960, 5760000},
      {28800, 172800000}, {2764800, 16588800000ull}}};
  for (const auto& [tick, frame] : answers) {
    CAPTURE(tick);
    CHECK(clock.frameAt(tick) == frame);
  }
  CHECK(clock.framesBetween(4, 16) == 72000);
}

TEST_CASE("rational frame clock retains fractional 44.1 kHz tick remainders") {
  // 44.1 kHz, 123 BPM and four sixteenth-note ticks per quarter note.
  const RationalFrameClock fullRatio(2646000, 492);
  const RationalFrameClock reducedRatio(220500, 41);
  // Answers computed independently with Python integer divmod and ties up.
  constexpr std::array<Answer, 12> answers{{
      {0, 0}, {1, 5378}, {2, 10756}, {3, 16134}, {4, 21512},
      {10, 53780}, {16, 86049}, {41, 220500}, {492, 2646000},
      {1000000, 5378048780ull}, {2833920, 15240960000ull},
      {100000000, 537804878049ull}}};
  for (const auto& [tick, frame] : answers) {
    CAPTURE(tick);
    CHECK(fullRatio.frameAt(tick) == frame);
    CHECK(reducedRatio.frameAt(tick) == frame);
  }
  // 96 hours of ticks reach the exact 44.1 kHz frame position. Repeatedly
  // adding the rounded first tick would have accumulated a large error.
  CHECK(reducedRatio.frameAt(2833920) != reducedRatio.frameAt(1) * 2833920);
}

TEST_CASE("rational frame clock expresses nonintegral tempo without float BPM") {
  // 129.5 BPM = 259/2; 48 kHz sixteenths have 1,440,000/259 frames per tick.
  const RationalFrameClock clock(1440000, 259);
  constexpr std::array<Answer, 11> answers{{
      {0, 0}, {1, 5560}, {2, 11120}, {3, 16680}, {4, 22239},
      {16, 88958}, {259, 1440000}, {186479, 1036794440},
      {186480, 1036800000}, {2983680, 16588800000ull},
      {1234567890, 6864006801544ull}}};
  for (const auto& [tick, frame] : answers) {
    CAPTURE(tick);
    CHECK(clock.frameAt(tick) == frame);
  }
}

TEST_CASE("rational frame clock rounds half frames up and derives intervals from endpoints") {
  const RationalFrameClock halves(1, 2);
  constexpr std::array<uint64_t, 9> expected{0, 1, 1, 2, 2, 3, 3, 4, 4};
  for (size_t tick = 0; tick < expected.size(); ++tick)
    CHECK(halves.frameAt(tick) == expected[tick]);
  CHECK(halves.frameAt(maximum) == 9223372036854775808ull);
  CHECK(halves.framesBetween(1, 2) == 0);
  CHECK(halves.framesBetween(2, 3) == 1);

  const RationalFrameClock thirds(1, 3);
  CHECK(thirds.framesBetween(1, 2) == 1);
  CHECK(thirds.frameAt(2 - 1) == 0); // Rounding the delta is a different contract.
  CHECK(thirds.framesBetween(2, 3) == 0);
  CHECK(thirds.framesBetween(0, 6) == 2);
}

TEST_CASE("rational frame clock long timelines remain within half a frame") {
  constexpr std::array<std::array<uint64_t, 2>, 4> ratios{{
      {220500, 41}, {1440000, 259}, {44100, 128}, {7, 13}}};
  for (const auto& ratio : ratios) {
    const auto numerator = ratio[0];
    const auto denominator = ratio[1];
    CAPTURE(numerator); CAPTURE(denominator);
    const RationalFrameClock clock(numerator, denominator);
    uint64_t previous = 0;
    // This oracle's products are bounded below 1.5e14. Its direct integer
    // quotient differs from the production wide-product algorithm and remains
    // exact well beyond binary32 integer-frame precision.
    for (uint64_t tick = 0; tick <= 100000000; tick += 9973) {
      const uint64_t product = tick * numerator;
      const uint64_t expected = (product + denominator / 2) / denominator;
      const uint64_t actual = clock.frameAt(tick);
      REQUIRE(actual == expected);
      CHECK(actual >= previous);
      const uint64_t scaled = actual * denominator;
      const uint64_t error = scaled > product ? scaled - product : product - scaled;
      CHECK(2 * error <= denominator);
      previous = actual;
    }
  }
}

TEST_CASE("rational frame clock accepts overflowing intermediate products with valid results") {
  CHECK(RationalFrameClock(maximum, maximum).frameAt(maximum) == maximum);
  CHECK(RationalFrameClock(maximum, 2).frameAt(1) == 9223372036854775808ull);
  CHECK(RationalFrameClock(maximum, 2).frameAt(2) == maximum);
  CHECK(RationalFrameClock(maximum - 1, maximum).frameAt(maximum) == maximum - 1);
  CHECK(RationalFrameClock(maximum - 1, maximum).frameAt(maximum - 1) == maximum - 2);
  CHECK(RationalFrameClock(2, 3).frameAt(maximum) == 12297829382473034410ull);
  CHECK(RationalFrameClock(3, 2).frameAt(12297829382473034410ull) == maximum);

  // The exact rational is M + 1/(M-2), but its rounded result is still M.
  // Rejecting an intermediate product or unrounded value above M is incorrect.
  const RationalFrameClock nearMaximum(maximum - 1, maximum - 2);
  CHECK(nearMaximum.frameAt(maximum - 2) == maximum - 1);
  CHECK(nearMaximum.frameAt(maximum - 1) == maximum);
  CHECK(nearMaximum.framesBetween(maximum - 2, maximum - 1) == 1);
}

TEST_CASE("rational frame clock reports only final rounded absolute overflow") {
  CHECK_THROWS_AS((void)RationalFrameClock(2, 1).frameAt(maximum), std::overflow_error);
  CHECK_THROWS_AS((void)RationalFrameClock(maximum, 2).frameAt(3), std::overflow_error);
  CHECK_THROWS_AS((void)RationalFrameClock(3, 2).frameAt(12297829382473034411ull),
                  std::overflow_error);
  const RationalFrameClock nearMaximum(maximum - 1, maximum - 2);
  CHECK_THROWS_AS((void)nearMaximum.frameAt(maximum), std::overflow_error);

  // 31 * tick / 2 = M + 0.5 exactly: ties-up itself must detect overflow.
  const RationalFrameClock halfOverflow(31, 2);
  CHECK(halfOverflow.frameAt(1190112520884487200ull) == maximum - 15);
  CHECK_THROWS_AS((void)halfOverflow.frameAt(1190112520884487201ull), std::overflow_error);
  // Endpoint semantics also reject an unrepresentable absolute endpoint,
  // even when the mathematical interval length would be small.
  CHECK_THROWS_AS((void)nearMaximum.framesBetween(maximum - 1, maximum), std::overflow_error);
}
