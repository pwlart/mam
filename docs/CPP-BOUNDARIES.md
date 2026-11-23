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
2. **Factory and offline session assembly.** `NodeFactory.cpp` now owns node
   creation and its dispatch through instrument parameter factories in
   `mam_nodes`. `SessionRuntime.cpp` owns session loading, metering, planning
   and rendering in `mam_session_runtime`. Their public headers retain types,
   declarations and small setters. `mam_session` remains the independently
   testable rack-planning target. These extractions preserve existing behavior,
   including the known session timing and loop-assembly limitations.
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

The factory and offline session boundaries are implemented; Graph and realtime
extractions remain open. Listing a header in a CMake target does not give that
target ownership of every inline implementation compiled by consumers.

| Responsibility | Ownership and allowed dependencies |
| --- | --- |
| Timing mathematics and basic engine contracts | Pure timing/core code; no device, file, renderer or application dependency |
| Instruments and effects | DSP implementation with core contracts; no CLI or device activation |
| Graph preparation and routing | Core/graph implementation with the node and effect contracts it uses |
| Node factory | `mam_nodes`: public `mam_core`; private `mam_dsp` and `mam_io` |
| Pure rack planning | `mam_session`: public `mam_core`; no session runtime or factory dependency |
| Offline session assembly | `mam_session_runtime`: public `mam_core`; private `mam_nodes` |
| File and device operations | Explicit I/O/platform targets; keep framework dependencies here |
| CLI and playback orchestration | Application targets depending on the required assembly/platform layers |

The factory is not pure core merely because its header lives under `src/core`.
`NodeFactory.cpp` includes `WiretapNode.hpp`, which reaches `AudioFileWriter`
and platform I/O, as well as concrete instrument factories. `mam_nodes` owns
those implementation dependencies; they no longer enter callers through the
factory header. `NodeFactory.hpp` includes complete `Node` and `NodeSpec` types
so a caller can construct a specification and destroy the returned
`std::unique_ptr<Node>` without including concrete instruments.

`mam_session_runtime` compiles offline assembly separately from pure rack
planning. Its public header still needs complete Graph, GraphSpec and SessionSpec
types because its exposed members contain them by value. Neither assembly
target depends on `mam_render` or `mam_app`; the aggregate and application
depend on these libraries. Keep that direction when extracting more code.

`mam_render` remains an `INTERFACE` convenience aggregate containing core, DSP,
I/O, nodes and session runtime. Compiled sources belong to their owning
implementation targets, not that aggregate. Apply the
shared warnings, sanitizer and language settings to every new compiled target.
Use `PUBLIC` dependencies when the public interface requires them and `PRIVATE`
for implementation-only dependencies; static-library consumers must still
receive the link dependencies needed to resolve implementations.

External factory callers must now link `mam_nodes`; callers of the moved
`SessionRuntime` methods must link `mam_session_runtime`. Existing consumers
of `mam_render` receive both. Including either public header alone no longer
supplies these implementations; there is no header-only implementation promise.

The `linkage.node_factory` and `linkage.session_runtime` checks compile their
public header first and link only the owning library. The factory consumer
constructs and destroys instruments and a disabled wiretap, and checks the
unknown-type result. The session consumer calls every moved method, including
header default arguments, with an empty session and an independent meter
fixture. These checks expose missing transitive link dependencies that broader
application test targets could conceal. Generated nonempty session and saved
PCM comparisons provide separate behavior evidence.

A portable mathematics target and its tests do not make the application or
offline renderer portable. Retain the normal application's platform checks
until file/device dependencies have been separated and actual rendering is
verified on another platform.

## Ownership and lifetime checks during moves

- Preserve public member order, nested types and method signatures when moving
  implementation. Keep default arguments on the public declarations and omit
  them from out-of-line definitions. The factory/session extractions do not
  introduce a private implementation, destructor or new move policy; consumers
  rebuild and link the owning libraries.
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
