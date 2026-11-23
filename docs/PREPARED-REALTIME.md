# Prepared realtime processing

The graph and session renderers prepare routing and storage before playback.
Their device-free `renderBlock` entry points are also used by CoreAudio, so
tests exercise the actual rendering path without opening a device. Offline
`Graph::process` remains available with its existing behavior.

## Prepare, feed, render, observe

1. Configure a stopped graph/session, including its nodes, routes and diagnostics.
2. Prepare with the actual rate, mono/stereo layout and maximum callback frames.
   Device adapters query `MaximumFramesPerSlice`; they do not assume 1,024 frames.
3. Build one owned event schedule and prime the exclusive first three seconds.
   Capacity is 2,047 commands for a graph and 16,383 for a session. Reject an
   overfull initial window before activation; later pressure retains events.
4. Render from fixed storage. A queue drain snapshots its producer head and
   fills a bounded span; new arrivals and cutoff events stay queued. Distinct
   commands are never deduplicated. Late timestamp groups retain their order
   at the next available block start and increment a late counter.
5. Poll numeric telemetry on the control thread. Formatting, console output and
   session metrics file writes happen there. Telemetry overflow drops diagnostic
   records and reports a counter; it does not drop musical events.
6. Stop/join feeding before stopping audio and releasing borrowed event strings.
   Read mutable graph statistics only after processing stops.

Single-graph transport retains its legacy bar rounding and continuous repetition.
Explicit-only graphs retain their exclusive 60-second demo horizon and last-event
period. Empty/frame-zero-only schedules now terminate preparation without a
zero-step loop. `ScheduledEvent::endFrameExclusive` bounds finite repetition
without building copies of the complete timeline.

## Graph preparation and sound compatibility

`Graph::prepareRealtime` compiles node/port routing, channel adaptation, dry
sends, mixer gains and event target lookup. It allocates graph and supported
effect storage without processing dummy samples. `processPrepared` accepts a
span and returns a status; structural edits invalidate its plan. Direct mutable
node access requires preparing the graph again after storage/configuration
changes. None of these operations may race processing.

Preparation rejects unsupported layouts, cycles, missing/duplicate node IDs,
invalid connections and allocating performance traces. Legacy offline cycle
fallback is unchanged. Existing effect dispatch, summation order, dry-send
suppression, zero sidechain and mixer sink fallback are retained. This does not
activate generic insert nodes that the legacy graph ignored.

Delay and spectral-ducker state are prepared for the actual channel layout.
Session bus duckers deliberately reset their state at every segment, matching
the old fresh-object behavior. Making them continuous would be an audible
change and belongs in another patch with a listening comparison.

One discovered compatibility gap is intentionally separate: valid manual
`xfader:<id>:x` automation was ignored by the old prefix check. This refactor
does not silently activate it; the existing LFO crossfader path is preserved.

Fresh valid nonempty graphs and positive-frame blocks have exact legacy parity
tests. Prepared empty graphs output silence, zero-frame calls do no DSP work,
and explicit preparation restarts specialized effect state. Unsupported capacity
or layout requests silence the supplied buffer and report a fault instead of
allocating larger storage in a callback. A node failure invalidates preparation;
its partially advanced state must not be treated as a completed render.

## What the harness establishes

The dedicated `mam_realtime_tests` executable instruments C++ `new`/`delete`,
including aligned forms, on the current thread. It measures the first block
without a processing warm-up, variable block sizes, mono/stereo effects, event
delivery and diagnostics. Positive controls demonstrate that the probes detect
allocations and console output. Assertions and diagnostic polling run outside
the measured scope.

These checks cover the supplied fixtures and observed C++ allocations and
stdout/stderr writes. They do not intercept every C allocator, file operation,
lock or system call; they do not certify arbitrary custom nodes or deadline
compliance. Live device/driver behavior, fractional transport timing
and storage-mutating live edits remain separate concerns. See
[testing](TESTING.md), [execution contracts](ENGINE-CONTRACT.md) and
[the roadmap](ROADMAP.md) for exact scope.
