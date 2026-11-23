# Rational timing groundwork

This is a preparation API for the timing migration, not a change to the audio
engine's current clock. `TransportGenerator` has moved into a compiled `.cpp`
implementation with its function body unchanged. Graph and session playback
still use the legacy transport calculations and integer-frame loop periods.
No existing render becomes timing-correct merely by adding this module.

## Exact frame conversion

`mam::timing::RationalFrameClock` in `core/RationalFrameClock.hpp` accepts a
positive integer ratio: frames per tick. The caller defines a tick's musical
meaning and supplies an exact ratio. There is deliberately no floating-point
BPM conversion or tempo-map policy in this first boundary.

| Operation | Contract |
| --- | --- |
| Construction | Both ratio terms must be positive; reduce by their GCD; reject zero with `std::invalid_argument` |
| `frameAt(tick)` | Round the absolute rational position to nearest, ties toward the later frame |
| `framesBetween(begin, end)` | Subtract rounded absolute endpoints; reject reversed intervals |
| Range | Accept any result whose rounded frame fits `uint64_t`; throw `std::overflow_error` otherwise |
| Intermediate arithmetic | A large product alone is not an overflow error; use exact quotient/remainder arithmetic |
| State | No mutable cursor or accumulated rounded step durations |

Each absolute result differs from the exact rational position by at most half
a frame, independently of duration. An interval is the difference of two such
positions, so its error can approach one frame. For a clock of one frame per
two ticks, positions 1 and 2 both round to frame 1: their interval is zero,
even though rounding one tick's duration independently would give one frame.
Small positive intervals can legitimately share a frame; ordering remains the
scheduler's responsibility. An interval whose absolute endpoints overflow is
rejected even when its mathematical length would fit.

The implementation uses standard C++20 64-bit integers. It separates whole
frames from the fractional part, then performs at most 64 binary division
steps. Modular addition avoids overflowing temporary products. This is a
control/preparation utility; it does not establish a callback deadline bound.

Examples of ratios, without passing through a floating-point BPM value:

- 48 kHz, 120 BPM, sixteenth notes: `RationalFrameClock(6000, 1)`.
- 44.1 kHz, 120 BPM, sixteenth notes: `RationalFrameClock(11025, 2)`.
- 44.1 kHz, 129.5 BPM, sixteenth notes: `RationalFrameClock(1323000, 259)`.

## Coordinated migration still required

The next behavior change must retain musical coordinates until an event's
absolute occurrence is converted to frames. For a constant-tempo event at tick
`e` in a loop of `L` ticks, repetition `k` is:

```cpp
clock.frameAt(k * L + e) // with checked tick arithmetic
```

It is not `clock.frameAt(e) + k * clock.frameAt(L)`: repeatedly adding a rounded
period loses fractional phase. Explicitly frame-authored events are different:
retain their integer offset `E` from `clock.frameAt(k * L)`. Do not silently
reinterpret their offsets as musical ticks.

| Consumer | Migration requirement |
| --- | --- |
| `offline/TransportGenerator` | Replace per-step truncation and repeated scans of previous bars with one validated tempo map; preserve locks/triggers and pattern addressing |
| `session/EventSchedule` | Preserve fractional occurrence phase while retaining ordering, bounded storage, admission, retry and overflow termination |
| `app/GraphPlayback`, `session/RackTimeline` | Derive event positions and loop boundaries from the same map, including tempo changes |
| `session/SessionRuntime` | Unify generation, looping and bar/step command conversion; currently a loop can be treated as a bar |
| `app/SessionPlayback` | Resolve musical commands after negotiating the actual device rate, using the same clock as rack events |
| `main.cpp` | Use the same map for trim windows, total duration and transport diagnostics |
| `core/TransportNode` | Decide whether to migrate or retire the unused emission scaffold; do not leave a competing timing formula |

Before that integration, specify how existing Float32 BPM and shaped swing
values become rational values. Decimal parsing, accepted ranges, duplicate or
unsorted tempo points, tempo changes at boundaries, odd resolutions, and tempo
map repetition need explicit policies. This module deliberately does not make
those compatibility decisions implicitly.

Acceptance needs long-duration tempo/swing and repeated-loop regressions,
half-frame absolute error bounds where applicable, device-rate conversion
fixtures, and saved-build render comparisons. Integer-aligned fixtures should
retain their audio; fractional timing changes need measured event deltas and
separate human listening review before production adoption.

## Focused checks without an Apple SDK

From the repository root, using the vendored doctest:

```sh
mkdir -p build/timing
c++ -std=c++20 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror \
  -Isrc -isystem vendor tests/test_main.cpp tests/test_rational_frame_clock.cpp \
  src/core/RationalFrameClock.cpp -o build/timing/mam_clock_tests
./build/timing/mam_clock_tests
```

The same cases are registered in the normal macOS test target. The command
above exercises only the new math boundary; it does not build MAM, test Apple
framework linkage or confer Linux support. Full default and appropriate
sanitizer presets are documented in [testing](TESTING.md). See
[implementation boundaries](CPP-BOUNDARIES.md) for the staged extraction plan.
