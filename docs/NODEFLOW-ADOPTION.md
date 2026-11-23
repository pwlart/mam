# What MAM takes from nodeFlow

The author's nodeFlow documentation informed this review. MAM adopts explicit
ownership, ordering, failure and overload contracts, not nodeFlow code or a
runtime dependency. The findings below describe MAM's existing implementation.

## The useful transfer

nodeFlow's strongest reusable idea is that ownership, ordering, failure and
overload are explicit contracts with executable checks. MAM benefits from that
discipline while keeping a sample clock and strict realtime constraints.

| nodeFlow practice / source | Application in MAM | Status |
| --- | --- | --- |
| Core owns state; adapters stay outside (`08_ARCHITECTURE`, `core/04`) | Separate graph/control ownership, offline workers, device I/O and callback processing | Runtime contract documented; larger core isolation remains planned |
| A run owns its data; failure publishes nothing (`core/02`, `10_ENGINE`) | Immutable render plans; completed artifacts only after validation; future graph swaps at block boundaries | File-job completion markers already exist; live graph swaps remain planned |
| Queue policy must be possible and observable (`11_CONCURRENCY` §5) | Reject or stage startup overflow; preserve ordered triggers; report lateness; coalesce meters only | Multi-rack startup/refill fixed with device-free regressions; single-graph migration remains |
| Deterministic clock and owned state (`core/01` §8, `core/05`) | Integer sample clock, per-node seeded RNG, serial/parallel/block-size parity | Sample-frame stem path verified; legacy RNG/transport still need work |
| Stable diagnostics and explicit recovery (`05_ERROR_REPORTING`, `core/03`) | Category/code/location plus user message; distinguish invalid input, timeout and comparison failure | Roadmap; current CLI messages are not a unified diagnostic contract |
| Debug assertions, TSan and isolated test layers (`core/05`, `11_CONCURRENCY` §6) | Separate debug/race presets; keep fixture tests independent of devices and production media | Debug/TSan presets implemented; existing suite passes |
| Build-time identity, including no-Git exports (`core/05`, `00_CHANGELOG`) | JSON build metadata; current revision/dirty state; same metadata in native result manifests | Implemented and regression-tested |
| Authoritative schemas and centralized conversion (`09_DATA_MODEL`, `04_FLOW_JSON_FORMAT`) | Generate docs/introspection from parameter contracts; validate units/layouts/versions at entry | Partial existing support; unify native/Python semantics in stages |
| Simulators reproduce real failures (`13_DEVICES`, `14_ROID_INTEGRATION`, `07_TESTING_A_CLIENT`) | Test plugin crash/hang, late reply, corrupt/short file, queue saturation and cancellation without hardware | Some file/process failures covered; plugin host itself remains unimplemented |
| Roadmap entries keep acceptance criteria (`12_ROADMAP`) | Each migration milestone has a runnable command, expected result and explicit remaining gap | Ordered milestones below |

## What was implemented in this pass

`mam --build-info` prints only JSON and opens no device. It includes the CMake
version, Git revision, dirty status, C++ standard, compiler, build type and
ASan/UBSan/TSan/LTO flags. The native version is defined once in CMake, not duplicated in the CLI banner. Source revision and
dirty status are resolved on every build, even after commits without a CMake
reconfigure. Unchanged metadata does not rewrite the generated header.

An archive with no repository reports `git_revision: "unknown"` and
`git_dirty: null`. A subtree inside another repository does not borrow that
repository's revision. Unknown identity is explicit. Build identity is metadata,
not a replacement for source/binary hashes or a clean-checkout requirement.

Native stem `result.json` now contains a `build` object. This is an additive
metadata change; audio samples, fades, DSP and existing releases are untouched.
The Python companion still records its own package/source hashes.

New `debug` and `tsan` configure/build/test presets are separate from default,
ASan/UBSan and release. TSan is rejected in combination with ASan, UBSan or LTO
instead of silently substituting options. No dependency was downloaded.

The new [execution contract](ENGINE-CONTRACT.md) and agent guidance distinguish
implemented guarantees, rules for new work and unresolved legacy behavior.

## Findings and current resolution

These are static code-review findings unless a test is named; no live audio
device was opened to reproduce a playback failure.

1. **Session startup waited for a stopped consumer — fixed 23 November.**
   The old active path filled a `SpscCommandQueue<16384>` (16,383 usable
   entries), retrying before `srt.begin()` could start the consumer. It now
   preflights the three-second window and uses one owned ordered cursor for
   startup/refill. Tests cover capacity, overlapping loops, future one-shots,
   retries, cancellation and late dispatch. The later duplicate branch was
   unreachable and is removed. The separate single-graph feeder remains open.
   See [refactoring details](REFACTORING.md).
2. **The callback path allocates and may log.**
   `RealtimeGraphRenderer::render()` creates split-offset storage and formats
   diagnostics; `Graph::process()` constructs per-port maps/vectors and can
   resize buffers. A no-allocation realtime guarantee is therefore not true
   of the current implementation. The new contract states this explicitly.
3. **Shared randomness undermines replay.** `src/core/Random.hpp` exposes a
   global `std::mt19937` and documents concurrent mutation as unsafe;
   `MamChipNode::process()` calls global `rand()`. A seed alone cannot guarantee
   identical per-node streams under a different scheduler. The current parity
   fixtures are not proof for every noisy rack.
4. **Offline worker lifecycle has weak boundaries.** `JobPool` has an unbounded
   vector, accepts submissions after stop, and directly invokes jobs without
   an exception containment boundary. Current scheduler batching limits normal
   submissions, but the class does not encode that assumption. Its lifecycle
   and failure contract should become testable before broader reuse.
5. **Graph guarantees are implicit.** `Graph` exposes mutable node access, and
   a topology cycle falls back to insertion order. Define which graph forms are
   supported and reject unsupported ones before processing; do not infer an
   arbitrary feedback model from the fallback. A debug single-node execution
   witness should cover actual processing, not unrelated completion cleanup.

## Ordered implementation milestones

| Priority | Bounded change | Acceptance evidence |
| --- | --- | --- |
| 1 | Multi-rack session milestone implemented; migrate the separate single-graph feeder next | Session regressions pass at/beyond capacity without lost/doubled/reordered events; apply equivalent acceptance tests to graph playback |
| 2 | Precompile graph routing/buffers off callback; move logging to bounded telemetry; add exact-scope debug re-entry guard | Allocation/fault-injection callback harness; no waits or I/O; late events counted; serial/parallel audio parity |
| 3 | Per-node seed/state and rational absolute beat→frame conversion | Same seed gives same node audio across block/thread counts; long-duration frame-error bounds; retain legacy golden comparisons |
| 4 | Bounded offline task lifecycle, cancellation and stable structured diagnostics | Reject after stop, join/drain semantics, worker-failure propagation, timeout tests, retry only where declared safe |
| 5 | One authoritative node/parameter schema and explicit format migration | Generated docs match schemas; unknown/narrowing/invalid-unit inputs fail clearly; old fixtures migrate intentionally |
| 6 | Plugin/device adapter with its own execution process and capability handshake | Real-format fake plugin supports crash, hang, late response and architecture mismatch tests; no plugin work in callback |

The session portion of the first milestone is implemented. Remaining feeder
migration and callback preparation are still required before claiming robust
unattended realtime playback. Each later step can land independently after its
acceptance checks. Software tests do not approve a musical direction.

## What should not be copied literally

- nodeFlow's blocking inbox, dynamic state copies and throwing nodes suit a
  control/dataflow engine. They are unsuitable inside an audio callback.
- Coalescing triggers is useful for recomputing a latest value; collapsing
  scheduled notes would erase rhythm. Use event-specific queue semantics.
- Float32 signal ports are not a template for frame counters. Float32 loses
  unit resolution above 16,777,216; MAM keeps uint64 frame positions and durable
  identifiers separate from sample values.
- Do not add gRPC, ZeroMQ or protobuf merely to mirror the architecture.
  Current local file/process boundaries already serve the offline workflow.
- nodeFlow's documented fallback to data-only unknown nodes is useful in an
  editor; a production audio render should refuse an unknown sounding node.
- Its older roadmap mentions fetching doctest; MAM retains its pinned local
  vendor copy and offline configuration/tests.
