# MAM execution contracts

This separates current guarantees from the rules new work must satisfy.
It adapts the ownership/testing discipline in the author's nodeFlow guides
to an audio engine. It does not claim that MAM's legacy realtime path already
meets a hard realtime contract. Detailed findings: [nodeFlow adoption](NODEFLOW-ADOPTION.md).

## Current execution boundaries

| Boundary | Current behavior | Constraint for changes |
| --- | --- | --- |
| CLI/control | Loads JSON, constructs graphs, starts/stops workers and devices | Validate before activation; do not mutate a running graph from another thread |
| Offline graph render | Serial/parallel schedulers process mutable node state | One invocation per node at a time; join work before reading final results |
| Native stem render | Fixed manifest order; bounded PCM reads/writes; integer frame placement | Preserve source windows, summation order and format contracts; publish completion last |
| Python jobs | Separate optional process, timeout, strict job schema, provenance/cache | File-based boundaries; no Python execution or package installation in the callback |
| Realtime callback | Legacy graph path currently allocates and can print diagnostics | New callback code must avoid allocation, locks, waits, file/network I/O and formatting |

The realtime row is an unresolved implementation gap, not a waiver. The
existing offline tests do not open an audio device and cannot establish glitch-
free playback, callback allocation bounds or safety of concurrent live edits.

## Node and event ownership

- `prepare()` and storage allocation belong before processing. State is owned
  by the node's processing path; control changes cross an explicit event or
  stopped-engine boundary. `reset()` must not race processing.
- Node re-entry is forbidden by the intended scheduler contract. A debug
  guard and direct re-entry regression remain planned; no new universal guard
  is installed by the documentation alone.
- SPSC means exactly one producer and one consumer. `SpscCommandQueue<N>` has
  `N-1` usable entries; `push()` returns false when full. Every producer must
  handle that return. Queue publication uses acquire/release ordering.
- Commands must arrive in nondecreasing absolute `sampleTime`; draining stops
  at the first future command. Borrowed `nodeId`/parameter-name strings must
  outlive every queued command that references them.
- A full startup queue cannot be waited on while its consumer is stopped.
  Preparation must reject an unsupported preload or use bounded staged feeding.
- Note/trigger events must not be coalesced like meters. Latest-value behavior
  is appropriate for telemetry only when the declared contract permits it.
  Report refused/late events with counters outside the callback.

### Multi-rack realtime session policy

`EventSchedule` owns the immutable base events and strings, plus one heap cursor
per event. It must outlive queued commands even after all one-shots are exhausted.
Events are ordered by absolute frame, target, parameter-before-trigger type,
parameter ID, value and base insertion order. Repetition does not reset this
ordering. Zero period means one-shot; uint64 timestamp overflow ends a repetition.

Before audio activation, the exclusive three-second initial horizon must fit
16,383 entries. Excess density is rejected; no consumer wait occurs. Later pumps
accept at most 16,383 events per invocation, leave a refused event pending and
check cancellation. A future one-shot cannot overtake earlier recurring events.
The producer's 20 ms polling interval is not a deadline guarantee.

Late commands are delivered at the next available block start, preserving their
original timestamp groups. Crossfader controls and parameters precede triggers
within each group. Every adjusted command increments a counter reported by the
control thread. Missed timing is audible degradation, not sample-accurate recovery.
Stopping joins the producer, stops audio, closes metrics, then releases borrowed
inputs. Startup failure never activates a partially primed queue.

These guarantees cover the multi-rack session route. The single-graph route has
not yet adopted this policy. Queue draining and callback execution remain
unbounded: the existing callback allocates, formats diagnostics and may write
metrics. The bounded producer does not establish a hard realtime callback.

## Time, randomness and types

`SampleTime` is a uint64 frame position. Keep frame indices/counts integer;
Float32 audio does not imply Float32 timing or identity. Beats, frames, seconds,
linear gain, dB, Hz and channel layouts are distinct quantities. Conversions
belong at validated boundaries, with explicit rounding and parity tests.

Use sample positions for DSP time; wall-clock time is for deadlines/diagnostics.
Legacy fractional-step truncation remains an open transport issue. Musical
session-command conversion also needs correction when actual device rate differs
from requested rate: its bar-frame metadata is used before rack rescaling.
New random
sources must have node-owned state and explicit seeds; global RNG and `rand()`
in legacy paths remain migration work. Preserve existing sounds through saved
fixtures/printed buses before changing a generator or seed derivation.

## Failure and completion

Offline exceptions stop a job; partial files without a valid successful result
manifest are not finished output. Existing destinations are refused. Native
stem results now include the binary's build identity; continue recording source,
recipe and audio hashes because a Git revision alone does not prove provenance.

Do not copy nodeFlow's throwing/blocking node pattern into an audio callback.
Validate predictable errors off the callback; an eventual realtime fault path
needs a bounded silence/bypass policy and a preallocated diagnostic channel.
That fault-containment mechanism is planned, not implemented here.

Distinguish request accepted, job completed, comparison passed, musical choice
accepted and publication authorized. Timeouts and process exits do not grant
artistic acceptance. Review and release decisions follow [contributing](../CONTRIBUTING.md) and [governance](../GOVERNANCE.md).

## Verification

Use generated fixtures to test node math without a device, then test scheduler
parity and failure paths. Assert frame boundaries and event ordering explicitly.
When threads are involved, use condition/barrier handshakes and bounded deadlines;
join/drain before final assertions. Repeated sleeps are not a completion proof.

Default/debug tests exercise behavior and assertions. TSan exercises races in
the paths run; ASan/UBSan address different failure classes. A clean sanitizer
run never proves untested realtime paths safe. Keep original failure logs and
reproduction commands. Production audio experiments remain separate and opt-in.
