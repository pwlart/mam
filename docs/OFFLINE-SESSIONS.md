# Offline session repetition

`SessionRuntime::renderOfflineWithLoop(frames, maxLoops, outStats)` repeats
offline session renders within an exact requested frame count. It returns
interleaved Float32 samples; one frame contains `channels` samples. Link
`mam_session_runtime`, or the `mam_render` aggregate, when using this API.

This is a stopped-engine, allocating operation. It is not a live looping
callback or a replacement for the separate [native PCM stem mixer](NATIVE-STEMS.md).

## Frame partition and placement

For a positive loop count, let `q = frames / maxLoops` and
`r = frames % maxLoops`. The first `r` segments contain `q + 1` frames;
the remaining segments contain `q` frames. Each nonempty segment calls
`renderOffline` and is copied immediately after the preceding segment, at
interleaved sample offset `framesRendered * channels`.

| Requested frames | Loop count | Segment lengths in frames |
| --- | --- | --- |
| 8 | 2 | 4, 4 |
| 10 | 3 | 4, 3, 3 |
| 3 | 5 | 1, 1, 1; empty segments are skipped |
| 0 | Any | Empty result; no segment is rendered |
| Positive | 0 | Requested-length silence; no segment is rendered |

There is no overlap-add, gap between segments or discarded remainder frame.
The returned vector contains exactly `frames * channels` samples for a valid,
successful call. A single loop uses the complete requested frame count.

The wrapper rejects zero channels with `std::invalid_argument` and a sample
count beyond `std::vector<float>::max_size()` with `std::length_error`, before
multiplication, statistics clearing or graph calls. These checks also apply to
zero-frame or zero-loop requests. A representable size can still fail allocation.
They do not establish general input validation for the planner, buses or the
standalone `renderOffline` method.

## Reset, events, effects and statistics

Each segment keeps the existing `renderOffline` lifecycle. Rendered racks are
prepared and reset; their commands use local frame positions starting at zero.
Rack offsets, gain, solo/mute selection and routes apply again to each segment.
Bus buffers are cleared and offline inserts are constructed as before. This
does not introduce a new reset policy for parameters, LFOs or instruments, or
guarantee identical repetitions for every stateful configuration. The native
noise reset contract is described in [randomness](RANDOMNESS.md).

Every segment ends at its assigned frame boundary. Events at or beyond its
exclusive end are not rendered in that segment. Instrument/effect tails are
truncated there; no crossfade, tail overlap or extra final tail is added. The
wrapper partitions whatever length the caller supplied, including any tail
frames already included by planning. It does not infer a musical loop period.

When provided, `outStats` is cleared on a successful call. With per-rack meters
enabled, it describes the **last nonempty segment**, not the complete repeated
result. Its peak/RMS values describe each rendered rack before rack/route gain
and bus processing. Disabled meters or no rendered segments leave it empty.
CPU measurement fields in `RackStats` are not newly populated by this wrapper.

## Compatibility and remaining limits

The factory/session source extraction preserved the previous implementation.
The subsequent placement correction intentionally changes affected repeated
output: the old code used a frame offset as an interleaved sample offset and
dropped integer-division remainders. Stereo segments could overlap, misalign
channels and leave a silent tail; short requests could render no segment at all.
Use historical builds or retained renders when reproducing that old output.
This software correction does not rerender or approve any production master.

Planning and CLI duration/loop-count selection retain their existing arithmetic.
`planTotalFrames` does not yet share one rational policy with commands, rack
overrides, loop periods and tail calculations. Offline session-level commands
are resolved but not applied. The CLI session WAV path also still selects a
two-channel output format even when the session declares a different layout;
mono/multichannel runtime behavior does not establish correct CLI export for
those layouts. These are separate issues, not promises made by this fix.

See the [timing groundwork](TIMING.md), [roadmap](ROADMAP.md),
[testing guide](TESTING.md) and [refactoring record](REFACTORING.md) for scope
and verification. Generated fixtures and numerical checks establish software
behavior, not physical playback quality or human musical acceptance.
