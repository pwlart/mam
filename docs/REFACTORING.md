# Engine refactoring

23 November 2026. Small, reviewable changes implement the community roadmap.
Structural extraction and behavior fixes remain separate commits.

## First slice: application and planning boundaries

- `app/CliOptions` owns parsing and entry selection without opening devices,
  mutating engine globals or exiting the caller. Existing flags and defaults
  are preserved; stricter numeric validation is a separate future change.
- `app/SessionPlayback` owns the active realtime session application path.
  The duplicate, unreachable session branch has been removed from `main.cpp`.
- `session/RackTimeline` prepares prefixed commands and loop metadata without
  file/device access. Existing transport rounding, parameter lookup, bar
  overrides and solo/mute decisions are retained.
- `mam_app` and `mam_session` are compiled CMake targets. This first boundary
  extraction does not yet isolate all CoreAudio dependencies or thin every
  remaining command handler out of main.

The first slice adds eight CLI and five rack-planning tests. The 51 native
CTest cases pass. Four generated offline renders match the previous clean
0.1.0 build sample-for-sample at 44.1/48 kHz; eight CLI routes preserve output
and exit codes, apart from the expected build identity/path differences.

## Second slice: bounded multi-rack session scheduling

`session/EventSchedule` owns base events and their target strings. A preallocated
heap merges recurring rack events and session one-shots in absolute frame order.
Startup and refill share the same cursor: previously the preload repeated three
loops, the feeder restarted at loop one, and future session commands could block
earlier rack events behind them. Racks without a transport now play each event
once. Overlapping tails and different rack periods remain globally ordered.

`session/SessionFeeding` defines admission and dispatch behavior. The first
three seconds must fit the queue's 16,383 usable entries; an unsupported initial
window fails before audio activation instead of waiting for a stopped consumer.
An exactly full but complete initial window succeeds. During playback a full
queue retains the next event for retry. Each producer pump has a fixed work
budget and observes cancellation. Base-event storage defaults to a limit of
one million entries; it does not expand copies of the complete performance.

Late events are applied at the next available block start and counted for
reporting on the control thread. Original timestamp groups keep their sequence;
within each group, crossfader controls and parameters precede triggers. Timing
is degraded after a missed deadline: this preserves delivery, not the original
waveform or spacing. Equal-key collisions within the schedule use base-event
insertion order, including overlapping repetitions; this can change the winning
ramp compared with the old repetition-first preload's inconsistent tie policy.

The feeder uses a joining, cancellable thread. It stops before the renderer;
the renderer stops before queue storage, event strings and graphs are destroyed.
The audio frame counter is reset before activation, and metrics are closed only
after audio stops. Device-free tests exercise the production cursor, admission
and dispatch helpers; actual CoreAudio start/stop is not exercised by these tests.

Validation on 23 November: all 67 native CTest cases pass in default, debug,
ThreadSanitizer, AddressSanitizer and UndefinedBehaviorSanitizer builds. The
optional Python preset passes its 68 CTest entries, including 69 pytest cases.
Four generated renders remain sample-for-sample identical to the clean 0.1.0
baseline, and eight CLI routes retain their normalized output and exit codes.
Repository hygiene, relative links, whitespace and local secret scans pass.

## Third slice: single-graph feeding

The remaining graph route now prepares one owned bounded schedule before audio
activation. Finite explicit-only schedules use an exclusive end frame rather
than pre-expanded copies; empty/frame-zero-only inputs cannot spin forever.
Transport retains its legacy loop arithmetic. Distinct events survive callback
dispatch, and live graph statistics are read only after processing stops.
The scheduling commit builds independently and passes all 75 native tests in
an isolated exported checkout.

## Fourth slice: prepared callbacks and diagnostics

Graph and session routing, event/split buffers and effect scratch are prepared
off-thread. Both real callbacks call the device-free `renderBlock` path; bounded
snapshot drains retain arrivals beyond the current batch. Diagnostics cross a
bounded numeric queue and are formatted/written by control-thread polling.
Capacity, format and node failures silence/report the block; CLI playback exits
after a fault. The device adapter queries the actual maximum callback size.

Generated tests compare prepared graph samples and session segment-reset
duckers with legacy behavior. A separate allocation harness measures the first
and variable blocks, actual instruments, effects, diagnostics and deferred metrics
files. Read [prepared realtime processing](PREPARED-REALTIME.md) for supported
behavior and the limits of these probes. This is not a universal node/device
deadline guarantee. Offline PCM and normal CLI characterization remain unchanged.

The complete fourth slice passes 110 native cases in default, debug, TSan,
ASan and UBSan builds. The Python preset passes 111 CTest entries including
69 pytest cases. The deferred metrics-file regression additionally passes in
all six configurations. Four offline renders match the saved pre-change build
exactly and eight CLI routes preserve normalized output/exit codes. Repository
hygiene, links, whitespace and local source/history secret scans pass.

## Fifth slice: bounded offline workers

`JobPool` now bounds pending admission and returns futures for completion,
values and exceptions. Full/closed admission fails immediately. Drain completes
accepted work; cancel-pending settles queued work with `Cancelled` while running
jobs finish normally. Concurrent shutdown callers serialize joining, and idle
completion includes callable cleanup. Scoped guards reject same-pool control
reentry from jobs and nested cancellation cleanup before it can deadlock.

The topological scheduler reserves its per-level future list before submission,
then waits for every accepted sibling and worker cleanup even when a job fails.
It no longer spins on a completion counter that a throwing job could abandon.
Failed renders discard partial output and release stale buffer references;
new calls rebuild topology and synchronize buffer channels. The contract is
documented in [offline workers](OFFLINE-WORKERS.md). Running jobs still need to
return; cancellation does not promise forced interruption or a deadline.

Validation: all 128 native CTest cases pass in default, debug, TSan, ASan and
UBSan builds. The optional Python preset passes 129 CTest entries, including
69 pytest cases. Eight saved-build render comparisons match sample-for-sample,
including serial/four-thread topology at 44.1/48 kHz; eight CLI comparisons
retain normalized output/exit codes. Source/history secret scans and repository
hygiene pass. A read-only CoreAudio probe exposed no devices to this process,
so no physical playback or device-setting changes were attempted.

## Sixth slice: owned native randomness

The previous seed option initialized an unused global `mt19937`; audible chip
noise used C `rand()` and clap always used a fixed private seed. Both native
noise instruments now own a versioned xorshift stream. A stable root/rack/node
derivation supplies their initial states independently of worker order, graph
insertion order and file paths. Root zero keeps instrument defaults. All graph
and session builders apply CLI-over-JSON precedence; live routing prefixes do
not alter local random identity. Seed inputs reject invalid types and overflow.

This intentionally changes chip noise and nonzero-seeded clap audio. The
unseeded clap waveform and non-noisy instruments are preserved. The clap's
existing negative-biased conversion remains a separate DSP correction. Node
reset replays its configured stream; unchanged session loop/reset differences
still apply. See [seed migration](RANDOMNESS.md) for algorithms and limits.

Validation: 148 native CTest cases pass in default, debug, TSan, ASan and UBSan;
the Python preset passes 149 entries including 69 pytest cases. The final
concurrent-render test also passes after its test-only barrier cleanup adjustment
in default/debug. Ten saved-build renders retain format/timing: six expected
compatibility cases are bit-exact and four expected noise migrations differ.
Eight CLI comparisons retain normalized output/exit codes apart from the updated
seed help. Hygiene, links, whitespace and local source/history secret scans pass.
No production audio or device playback is part of this verification.

## Seventh slice: compiled transport boundary and exact clock groundwork

`TransportGenerator` now has a declaration header and one compiled implementation
in `mam_core`. The function body is identical to the preceding revision; the
mechanical commit preserves event ordering and all legacy timing behavior.

The separate `RationalFrameClock` converts exact positive tick ratios to absolute
frames with nearest/ties-up rounding and explicit overflow handling. Standard
C++20 integer arithmetic avoids overflowing intermediate products without
floating-point math or compiler-specific wide types. Intervals use rounded
absolute endpoints so their starting fractional phase is retained. Eight unit
cases cover long timelines, nonintegral tempo ratios and full-range boundaries.

This remains groundwork: production transport and looping do not use the new
clock yet. The [timing plan](TIMING.md) identifies the event, loop, trim and
session conversions that must migrate together. The [C++ boundary policy](CPP-BOUNDARIES.md)
defines selective source extractions, target dependencies and lifetime checks.

Validation on 23 November with Apple Clang 21.0.0 and CMake 4.3.3: the extraction
alone passes all 148 preceding native cases; the expanded 156-case suite passes
in default, ASan and UBSan configurations. Ten generated two-second renders at
44.1/48 kHz match the saved clean build sample-for-sample, including fractional
tempo/swing and seeded noise. Four additional serial/four-worker comparisons
match, and eight CLI routes retain normalized output and exit codes. Repository
hygiene, links, whitespace and local source/history secret scans pass.

These Mac results supersede the earlier environment's outstanding native checks.
The debug, TSan, Python and release presets were not rerun for this preparation
slice, which adds no concurrency or Python behavior. Physical audio playback and
production music are outside this verification; no accepted master was rerendered.

## Eighth slice: compiled factory and session assembly

`NodeFactory` now exposes a small declaration header; the original construction
body lives in `NodeFactory.cpp`, owned by `mam_nodes`. This assembly target links
core, DSP and I/O explicitly. The factory no longer brings concrete instruments,
JSON parsing and audio-file framework headers into every caller. The extraction
also replaces two hidden include dependencies with explicit ParamMap and macOS
`unistd.h` includes where they are used.

The five substantial `SessionRuntime` methods now live in `SessionRuntime.cpp`,
owned by `mam_session_runtime`. Their bodies, public value types, defaults and
implicit move/destruction behavior are unchanged. `mam_session` remains the
pure rack-planning target; neither implementation library depends on application
or render aggregates. Normal `mam_render` consumers receive both libraries;
direct users must link their owning target. See [C++ boundaries](CPP-BOUNDARIES.md).

Two standalone executables include the public headers first and link only the
owning library, checking construction/destruction and all moved session methods
without borrowing application dependencies. The factory checkpoint passes 157
native cases; the complete extraction passes 158 in default, ASan and UBSan.
The release/LTO configuration builds successfully. Twenty generated renders
(ten graph and ten session) match saved 61b6cf4 output bit-for-bit at 44.1/48 kHz.
Four worker comparisons and eight CLI comparisons retain existing behavior;
source/history secret scans, repository hygiene and whitespace checks pass.

The session comparisons exercise gain/offset, solo/mute, spectral bus and loop
paths. Compatibility is not a correctness claim for legacy loop placement:
review found an existing frame/interleaved-sample indexing error and dropped
remainder frames in `renderOfflineWithLoop`. Both remain unchanged and are
recorded as a separate [roadmap issue](ROADMAP.md). No production music,
physical playback, timing integration or Python behavior is part of this slice;
debug, TSan and Python presets were not rerun.

## Ninth slice: correct offline session loop placement

`renderOfflineWithLoop` now concatenates segments at interleaved sample offsets
derived from frame positions. It divides the complete requested length across
the loop count, assigning one extra frame to each of the first remainder
segments. Requests shorter than the loop count render their nonempty one-frame
segments. Zero loops still return requested-length silence; zero frames render
nothing. Invalid channels and unrepresentable sample counts fail before graph
calls or statistics changes. See [offline session repetition](OFFLINE-SESSIONS.md).

This intentionally fixes the preceding slice's preserved overlap and lost-frame
defects. Graph reset, local events, rack offsets/gains, hard segment-end truncation
and last-segment pre-gain statistics retain their existing policy. There is no
new crossfade, tail overlap, loop-period inference or rational-clock integration.

The first eleven regression cases exposed seven failures against the old
implementation; a twelfth case adds a processing-block boundary check. All 170
native cases pass in default, ASan and UBSan, and the Release/LTO build succeeds.
Of 22 generated saved-build comparisons at 44.1/48 kHz, 18 remain bit-exact and
four loop outputs change intentionally. Every corrected loop output matches an
independent concatenation of nonloop segments rendered by saved 54f3763, including
odd total lengths. Eight CLI checks remain unchanged and four serial/four-worker
comparisons remain exact. Debug, TSan and Python presets were not rerun; this
slice changes neither concurrency nor the Python bridge. No physical playback
or production audio is part of this software correction.

## Tenth slice: preserve CLI session export layout

Offline session export now uses the runtime channel count for file metadata
and frame reporting. Mono samples retain their intended duration instead of
being reinterpreted as stereo pairs; odd mono frame counts export successfully.
The CLI rejects parsed session and bus layouts outside mono/stereo before
loading racks. Some instruments/effects assume these layouts, so this does not
extend arbitrary multichannel DSP support. The generic runtime/writer APIs,
duration planning, sample synthesis and graph-only CLI path remain unchanged.
See [offline sessions](OFFLINE-SESSIONS.md) for compatibility and input limits.

Twelve independent CLI regressions generated nine expected failures and three
passing stereo controls against saved aa29cde. They inspect RIFF data directly,
compare with a separate graph waveform, verify explicit loop boundaries and
exercise invalid layouts/overwrite refusal. The first full runs exposed the
test reader's assumption about padding after a final odd-sized PCM24 data
chunk. It now accepts CoreAudio's terminal-data convention while still rejecting
internal missing padding and incomplete frames; original failures were retained.

All 182 native cases pass in default, ASan and UBSan; Release/LTO builds.
Twenty-two saved-build stereo graph/session render pairs remain bit-exact at
44.1/48 kHz, including loop, bus, offset/gain and worker paths. New CLI fixtures
cover 48 kHz WAV, not AIFF/CAF. Debug, TSan and Python presets were not rerun;
this slice changes no concurrency, Python bridge or production audio.

## Remaining scope

Broader node/device coverage, rational transport time, clap noise bias,
manual crossfader automation and structured diagnostics remain roadmap items.
Musical session commands still have a clock-conversion gap if the actual device
rate differs from the requested rate. These changes do not rerender accepted
production masters.
