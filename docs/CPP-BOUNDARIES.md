# C++ source and target boundaries

MAM uses C++20. Headers describe interfaces and the definitions that callers
must see; source files own ordinary implementation. Move code to clarify
dependencies and ownership, not to meet a file-size quota. A header-to-source
move alone does not establish faster builds, faster DSP or realtime safety.

This policy complements [contributing](../CONTRIBUTING.md), the
[execution contract](ENGINE-CONTRACT.md) and [refactoring record](REFACTORING.md).
The [roadmap](ROADMAP.md) tracks behavior changes and their acceptance evidence.

## What belongs where

| Keep in `.hpp` | Put in `.cpp` |
| --- | --- |
| Public types, declarations, enums and constants | Ordinary parsing, validation and orchestration implementations |
| Templates that must be instantiated by callers | Non-template graph preparation, routing and render methods |
| Small accessors and deliberate inline arithmetic | File/device adapters, diagnostics and platform details |
| `constexpr` definitions needed for constant evaluation | Private helpers used by one translation unit |

Keep headers self-contained. Include the declarations a header actually uses;
do not rely on an unrelated include to supply them. Forward declarations can
reduce coupling when a declaration only needs a pointer or reference, but
by-value members and inheritance require complete types. Do not introduce a
heap-allocated private implementation solely to hide a short class definition.

Templates such as `SpscCommandQueue`, `TelemetryQueue`, `ModMatrix` and
`ParameterRegistry` remain visible to callers. The templated `JobPool::submit`
and callable task adapter, `EventSchedule::pump` and session event-dispatch
helpers also need visible definitions unless an explicit, deliberately limited
instantiation contract replaces them. Their surrounding non-template lifecycle
or planning methods may move independently.

An `.inl` file is still included implementation, not a compiled boundary.
`PreparedGraph.inl` contains non-template Graph methods and can move with them.
Small `NoiseRandom` arithmetic can remain inline; a larger implementation move
must preserve its documented integer stream and seed derivation. Measure any
performance claim separately from the organizational change.

## Extraction order

1. **Transport generation.** `TransportGenerator.hpp` now exposes declarations;
   `TransportGenerator.cpp` holds the existing implementation in `mam_core`.
   This first mechanical extraction preserves timing, event ordering, ties and
   existing limitations. It is not the rational-clock migration.
2. **Factory and offline session assembly.** Move non-template node creation,
   JSON parameter handling and `SessionRuntime` loading/rendering out of their
   headers. Establish their real DSP and I/O dependencies before assigning the
   new sources to targets. Keep independently testable rack planning separate.
3. **Graph implementation.** Move preparation, topology, event lookup and
   routing methods from `Graph.hpp` and `PreparedGraph.inl` into `Graph.cpp`.
   Preserve prepared-state invalidation, move behavior and the exact ordering
   of DSP calls and floating-point sums.
4. **Realtime renderer and device adapters.** Move ordinary graph/session
   renderer methods into source files, retaining the required queue-adapter
   templates. Keep the device-free `renderBlock` path shared with the callback;
   then isolate CoreAudio activation and format negotiation behind the platform
   boundary in a separate reviewable step.

Do one bounded extraction at a time. Finish its target dependencies and
verification before starting a second large class. Existing CLI parsing and
`SessionPlayback` source files are useful boundaries to extend, rather than
recombining application orchestration with DSP.

## Target ownership

These are dependency rules, not a claim that all target separation is complete.
Listing a header in a CMake target does not give that target ownership of every
inline implementation compiled by consumers.

| Responsibility | Ownership and allowed dependencies |
| --- | --- |
| Timing mathematics and basic engine contracts | Pure timing/core code; no device, file, renderer or application dependency |
| Instruments and effects | DSP implementation with core contracts; no CLI or device activation |
| Graph preparation and routing | Core/graph implementation with the node and effect contracts it uses |
| Node factory | Assembly target depending on core, DSP and required I/O implementations |
| Offline session assembly | Session/render assembly depending on graph, factory and offline rendering |
| File and device operations | Explicit I/O/platform targets; keep framework dependencies here |
| CLI and playback orchestration | Application targets depending on the required assembly/platform layers |

The factory is not pure core merely because its header lives under `src/core`.
Today `NodeFactory.hpp` includes `WiretapNode.hpp`, which reaches
`AudioFileWriter` and platform I/O, as well as concrete instrument factories.
Moving that code into `mam_core` without restructuring dependencies would make
the supposed core library depend on DSP and I/O. Prefer a compiled assembly
target linking those implementations, and make its consumers depend on it.

Similarly, `mam_session` currently compiles pure rack planning. Adding all of
`SessionRuntime` to it changes its dependency contract: either declare the new
assembly dependencies or keep a separate planning target. Avoid a cycle where
core depends on a renderer that already depends on core.

`mam_render` can remain an `INTERFACE` convenience aggregate. Compiled sources
belong to their owning implementation targets, not that aggregate. Apply the
shared warnings, sanitizer and language settings to every new compiled target.
Use `PUBLIC` dependencies when the public interface requires them and `PRIVATE`
for implementation-only dependencies; static-library consumers must still
receive the link dependencies needed to resolve implementations.

A portable mathematics target and its tests do not make the application or
offline renderer portable. Retain the normal application's platform checks
until file/device dependencies have been separated and actual rendering is
verified on another platform.

## Ownership and lifetime checks during moves

- An out-of-line destructor used with an incomplete `unique_ptr` pointee needs
  its definition where the pointee is complete. Declaring a destructor also
  suppresses implicit move generation. Preserve Graph's required move
  construction/assignment explicitly, with a justified exception specification;
  racks are stored in movable containers.
- Prepared Graph lookups borrow IDs through string views. Preserve their
  ownership and validity after moves, including short strings, and retain
  the existing move/lifetime regressions. Reconfiguration still invalidates
  prepared state; moving methods must not bypass that contract.
- Playback borrows graphs, queues and command target strings. Stop and join
  feeding, stop audio and perform final diagnostic polling before releasing
  those owners. Deferred telemetry does not extend a borrowed string's lifetime.
- Offline tasks can borrow renderer state. Every accepted task and callable
  cleanup must finish before failure releases that state. Preserve this ordering
  when moving `JobPool` or scheduler implementation into source files.

## Keep structural and timing changes separate

A mechanical commit preserves routing, summation order, command tie ordering,
rounding, reset behavior and errors. Compare against a saved pre-change build;
do not rewrite expected samples simply because code moved. A discovered bug
belongs in a separate behavior patch with a reproducer and explicit migration
notes, even when its fix is small.

The new rational frame-clock foundation is not yet connected to production
audio scheduling. Integrating only event generation would leave independent
bar, loop, trim/end and session-command conversions inconsistent. The timing
migration must define BPM conversion, rounding, swing and overflow behavior,
then apply one clock policy across those paths and negotiated device rates.
Track that integration and long-duration frame-error evidence through the
roadmap and timing contract. Do not describe mathematical unit tests as proof
that existing rendered timing or live playback has changed.

## Local engineering handoff

1. Read `AGENTS.md`, `README.md`, `CONTRIBUTING.md`, `docs/TESTING.md`, the
   relevant execution contract and roadmap. Check the current branch, working
   tree and source identity; agree file ownership before parallel edits.
2. Keep the saved baseline and use generated fixtures without private recordings
   or a device. Record intended compatibility and the specific behavior tested.
3. Run the applicable full Mac presets: default configure/build/CTest, debug
   and TSan for concurrency, ASan/UBSan for memory/buffer/DSP changes, and a
   release build. Use the Python preset when its bridge or examples are affected.
   A portable mathematics-only run does not replace these application checks.
4. Run `git diff --check`, `python3 tools/check_repository.py` and
   `bash tools/check_secrets.sh`. Keep build trees and generated audio outside
   Git. Check documentation links and target membership after file moves.
5. Record source/build identities, platform/tool versions, actual checks and
   unrun checks. Keep mathematical correctness, PCM compatibility, physical
   playback and human musical acceptance as separate evidence.
