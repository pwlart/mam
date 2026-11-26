# Community roadmap

0.1.0 is a community preview. These are proposed priorities, not promised dates.
Issues should carry a small reproducer and the acceptance evidence below.
See [governance](../GOVERNANCE.md) for how changes are reviewed.

| Priority | Work | Done when |
| --- | --- | --- |
| P0 | Extend realtime coverage and live-device verification | Exercise every supported node/configuration, C allocators, waits and deadline behavior; validate CoreAudio negotiation and fault recovery on hardware |
| P1 | Rational beat-to-frame timing | Long-duration tempo/swing cases have explicit frame-error bounds; fractional rates no longer accumulate truncation |
| P1 | Unify offline session duration planning | Loop counts, rack overrides, command lengths and tails share a declared frame policy; short/generated sessions prove exact planned end frames |
| P1 | Stable structured failure diagnostics | Categorize invalid input, task failure and cancellation consistently; report locations and declare safe retry boundaries |
| P1 | Apply graph validation consistently offline | Prepared realtime graphs reject cycles/unknown links; replace the remaining offline insertion-order fallback with an explicit policy |
| P1 | Repair manual session crossfader automation | Activate valid targets in a separate behavior patch with event/DSP comparisons; preserve the established LFO path |
| P2 | Correct legacy clap noise bias | Compare the existing approximately [-1,0) mapping with bipolar/DC-corrected output in a separate sound migration |
| P2 | One parameter/schema authority | Generated docs and validation share units, IDs, ranges and migration rules |
| P2 | Portable offline core | CoreAudio separated from file/DSP libraries; Linux CI exercises actual offline rendering before claiming support |
| P2 | Isolated plugin host | Fake plugin fixtures cover crash, hang, late response and architecture mismatch outside the audio callback |

## Completed since 0.1.0

Rational timing has a [tested math boundary](TIMING.md) in preparation. It is
not yet used by transport or playback; the P1 timing item remains open.
[C++ implementation extraction](CPP-BOUNDARIES.md) proceeds separately from
sound/timing changes, with explicit target dependencies and local validation.

- Corrected CLI session export to retain mono/stereo channel layout and report
  the actual frame count. Unsupported parsed session/bus channel counts fail
  before graph loading. This closes the export-layout part of the planning
  item; duration planning and strict input validation remain open.
- Extracted CLI parsing, realtime session orchestration and pure rack planning
  into explicit targets, preserving offline output and existing CLI behavior.
- Moved node construction and offline session assembly into compiled
  `mam_nodes` and `mam_session_runtime` targets. Public headers retain their
  declarations/value types; isolated consumers verify implementation link
  dependencies. The extraction preserves existing sound and timing behavior.
- Corrected offline session repetition in a separate behavior patch: complete
  interleaved frames are concatenated without overlap and remainder frames are
  retained. Output-size guards precede allocation; the existing reset, hard-tail
  and last-segment meter policy is explicit. See [offline sessions](OFFLINE-SESSIONS.md)
  for intentional output changes and the remaining planner/export limitations.
- Bounded multi-rack session startup/refill with one owned cursor, explicit
  initial-window rejection, ordered retries and reported late-event recovery.
  Capacity boundaries, different loop periods, one-shots, cancellation and
  dispatch are covered without an audio device. This completes the session
  portion of the startup milestone. See [refactoring evidence](REFACTORING.md).
- Migrated the remaining single-graph feeder, with finite cursors, initial
  admission before playback and no lost distinct events or zero-step loops.
- Prepared graph/session routing and buffers off-callback, bounded snapshot
  draining, and moved diagnostics/metrics to control-thread polling. A dedicated
  device-free harness measures first/variable blocks, C++ allocation, output and
  telemetry saturation. See [prepared realtime scope](PREPARED-REALTIME.md).
- Bounded offline worker admission, futures for completion/failure, explicit
  drain/cancel-pending shutdown and joined ownership. Parallel render failures
  settle sibling tasks before releasing inputs; retries rebuild render state.
  See [worker lifecycle](OFFLINE-WORKERS.md).
- Node-owned native noise, stable per-node seed derivation and graph/session
  seed propagation. Invalid seed values fail explicitly; reset and scheduler
  comparisons use generated noisy fixtures. See [seed migration](RANDOMNESS.md)
  for intentional chip/seeded-clap changes and preserved unseeded clap behavior.

The prepared paths do not establish universal realtime safety or a hardware
deadline guarantee. Keep timing and manual crossfader changes separate so
their audio differences remain reviewable. Worker cancellation does not interrupt
running code or guarantee a shutdown deadline; broader node/device coverage and
structured diagnostics remain open.

The earlier session extraction preserved the then-existing loop placement
defects; the subsequent correction is a separate sound migration. Duration and
loop-count planning still use legacy arithmetic. Those issues remain separate
from corrected frame placement/export layout and the unintegrated rational-clock API.

## Next implementation sequence

1. Define and test one offline duration plan covering rack overrides, commands,
   offsets, loop counts and tails. Keep the intended audible differences explicit.
2. Integrate the rational clock across event generation and all dependent bar,
   loop and end calculations. Include negotiated device rates before making a
   live timing claim; do not migrate only one rounding site.
3. Reject invalid offline graph topology consistently, with actionable errors.
   Then repair manual crossfader automation in its own behavior patch.
4. Continue mechanical Graph and realtime implementation extraction separately,
   following [C++ boundaries](CPP-BOUNDARIES.md). Keep templates and deliberately
   small inline primitives in headers; preserve PCM in extraction commits.

Realtime hardware verification remains P0 alongside this software sequence;
it requires a device session and cannot be replaced by offline regressions.
Hosted CI also remains a release gate: runner-capacity cancellation is not a
test result. Validate the published revision, then review integration into main
and required checks as separate maintainer actions. See [CI recovery](CI.md).

## Useful first contributions

- Improve one DigitalMania recipe with a clear parameter explanation and a
  generated-fixture test; preserve attribution and sample-free provenance.
- Add malformed WAV/job cases or a small diagnostic improvement.
- Verify the getting-started steps on a second Mac and report exact versions.
- Make generated players more accessible without adding services or trackers.
- Turn one timing/queue finding into a deterministic regression before its fix.

## Community infrastructure proposals

1. Maintain a short issue backlog with labels for bug, docs, DSP, tests and
   good-first-issue. Each item needs scope, an owner only when volunteered,
   and a runnable acceptance check.
2. Enable required CI checks and protected main after the initial clean import.
   The workflows are supplied; repository settings still require maintainer setup.
3. Publish release source/checksums and optional generated demo audio separately
   from Git. Document the exact source revision and tool versions for each asset.
4. Add reviewers as contributors build experience. Publish decisions and
   compatibility notes; avoid assuming a larger maintainer team already exists.
5. Add fuzzing for file/JSON readers and scheduled dependency/license review.

The full DigitalMania production, reference recordings and personal archive are
outside this community repository. Musical acceptance and software correctness
remain separate judgments.
