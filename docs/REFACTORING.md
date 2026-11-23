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

## Remaining scope

Broader node/device coverage, rational transport time, per-node randomness,
manual crossfader automation and worker-pool lifecycle remain roadmap items.
Musical session commands still have a clock-conversion gap if the actual device
rate differs from the requested rate. These changes do not rerender accepted
production masters.
