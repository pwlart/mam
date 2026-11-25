# Local C++20 build and regression tests

From the `mam` directory:

```sh
cmake --preset default
cmake --build --preset default --parallel
ctest --preset default --output-on-failure
```

Requires macOS, a C++20 compiler/SDK, CMake 3.21+ and Ninja. Verified on
20 November 2026 with Apple Clang 21.0.0, CMake 4.3.3 and Ninja 1.13.2,
Apple Silicon. No package download, Python, account, audio device or source
recording is required to build and run these tests. The test framework is
doctest 2.5.3 in `vendor/doctest`; CTest discovers individual C++ test cases.

| Preset | Directory | Purpose |
| --- | --- | --- |
| `default` | `build/default` | Optimized debug information, warnings as errors, tests |
| `asan` | `build/asan` | Debug + AddressSanitizer, tests |
| `ubsan` | `build/ubsan` | Debug + UndefinedBehaviorSanitizer, fail on first finding |
| `release` | `build/release` | Release + checked LTO support, tests disabled |
| `python` | `build/python` | Native suite plus optional Python worker/integration tests |
| `debug` | `build/debug` | Assertions enabled, no sanitizer |
| `tsan` | `build/tsan` | ThreadSanitizer; separate from ASan/UBSan/LTO |

```sh
cmake --preset asan
cmake --build --preset asan --parallel
ctest --preset asan --output-on-failure

cmake --preset ubsan
cmake --build --preset ubsan --parallel
ctest --preset ubsan --output-on-failure

cmake --preset release
cmake --build --preset release --parallel
```

Sanitizer compile/link options reach the libraries, CLI, tests and parameter
documentation generator. `BUILD_TESTING=OFF` excludes doctest entirely.
Normal builds do not modify tracked documentation. To generate parameter
tables, run `cmake --build --preset default --target docs`; review
`build/default/docs/ParamTables.md` before updating the tracked document.

## What the suite checks

The current development suite has 170 native CTest cases: 153 doctest units and 17
integration/tooling checks (the released 0.1.0 snapshot had 38 native cases).
The optional `python` preset adds one CTest entry
running 69 worker, DSP and DigitalMania pytest cases:

```sh
cmake --preset python
cmake --build --preset python --parallel
ctest --preset python --output-on-failure
```

The native reader tests exact seeks, partial/chunk boundaries, invalid files,
nonfinite output and overwrite refusal. Python integration covers native frame
placement, fades, overlap, PCM conversion, subprocess timeouts and processed
stem parity. See [the companion guide](../python/README.md) for environment
configuration. To run these integration cases against an instrumented MAM,
configure `asan` or `ubsan` with `-DMAM_ENABLE_PYTHON_TESTS=ON` before building.

- Parameter ramps finish on the intended sample; retargeting, immediate,
  exponential and capacity cases keep their defined behavior.
- Command queues preserve block cutoffs, wraparound and event order, including
  100,000 events sent by a separate producer thread. Head publication now
  uses release/acquire synchronization; this is not a full realtime race audit.
- CLI parsing and rack planning run without devices; existing defaults, aliases,
  failure exits, prefixing, solo/mute and legacy transport rounding are covered.
- Standalone factory and session consumers include their public header first
  and link only `mam_nodes` or `mam_session_runtime`. They exercise construction,
  destruction, moved methods and default arguments without borrowing the
  application's aggregate dependencies. Wiretap is disabled; no file/device is
  opened. These link checks complement the generated render comparisons.
- Twelve offline session-loop cases check literal mono/stereo sample sequences,
  odd/even and remainder lengths, skipped empty repetitions, 1024-frame block
  boundaries, reset/event cutoff, repeated offset/gain and final-loop meters.
  Invalid channels and unrepresentable sample counts fail before state mutation.
  The [offline session contract](OFFLINE-SESSIONS.md) records intentional changes
  from legacy overlapping writes and discarded remainder frames.
- The session cursor merges different loop lengths, overlapping tails and future
  one-shots; ties use deterministic base insertion order. Capacity boundaries
  include the real 16,383-entry queue, explicit initial-window rejection, retry
  without loss and cancellation with a producer-thread handshake.
- Callback event dispatch retains original late timestamp groups, parameter
  values seen by successive triggers and on-time xfader/parameter/trigger phases.
  Late counts and delivery occur once. These pure helper tests do not open
  CoreAudio or establish a callback allocation/deadline bound.
- Prepared graph and session renderers share their actual `renderBlock` path
  with a device-free harness. It checks exact routing/DSP parity, capacity/fault
  behavior, bounded queue snapshots, telemetry saturation and first/variable
  mono/stereo blocks with actual instruments, delay and spectral ducking.
- The dedicated `mam_realtime_tests` executable instruments C++ allocation/free
  and captures console output, with positive controls. Session metric files stay
  empty until control polling. These checks do not intercept every C allocator,
  syscall or lock and do not prove arbitrary-node or hardware deadline safety.
- Offline workers have explicit full/closed admission, task exception and
  drain/cancel-pending tests. Concurrent joining, cancellation cleanup, retained
  futures and nested cleanup reentry use handshakes and bounded test deadlines.
  Render failures must settle active/pending siblings, clear partial audio and
  support retry with a changed graph, block size or channel layout.
- Native noise tests lock integer/hash vectors and the unseeded clap waveform;
  multi-chip/clap fixtures compare reset/replay, independent concurrent graphs,
  stable node/rack identity, 17/64/256/1024-frame partitions and serial/four-worker
  rendering at 44.1/48 kHz. CLI audio tests check real graph/session seed precedence,
  while input tests reject malformed/overflow seeds. The prepared instrument
  harness observes zero C++ allocation/free for the measured noisy blocks.
- Kick synthesis stays finite, decays, responds to velocity and resets
  reproducibly. Meter and SHA1 results have independent known answers.
- Transport locks precede same-sample triggers; exact 48 kHz beat positions,
  swing and tempo transitions are checked. Fractional-step timing at 44.1 kHz
  remains a documented migration issue, not a passing accuracy claim.
- The separate rational-clock cases check half-frame error bounds through long
  timelines, ties-up rounding, endpoint intervals, nonintegral rational tempo
  and full-range arithmetic with valid or overflowing final results. This API
  has not yet replaced the transport generator or integer loop scheduler.
- Float32 WAV samples and channel order round-trip exactly over multiple
  65,536-frame write chunks; integer exports stay within one LSB. Malformed
  inputs fail before file creation. An existing destination survives unchanged.
- Real two-second acid/sidechain racks match across baseline, serial topology
  and four-thread topology renderers at both 44.1 and 48 kHz, including tails.
- The parity script rejects missing, malformed, duplicate and mismatched hashes,
  failed render processes and missing audio files. A positive control verifies
  it can also succeed. The old stdout-only parser falsely accepted empty hashes.
- Strict schema requests fail explicitly, and vendored files match SHA256
  provenance. The old no-op schema shim has been removed; native validation is still unavailable.
- Build identity changes after a commit without reconfiguration, reports dirty
  state, remains stable when unchanged and reports unknown for exported trees
  rather than borrowing a parent repository's revision. The CLI emits pure JSON.

Build identity fixture tests use local Git repositories in the build directory;
Git must be available for those tests. They do not change project history.
For concurrency changes:

```sh
cmake --preset debug
cmake --build --preset debug --parallel
ctest --preset debug --output-on-failure
cmake --preset tsan
cmake --build --preset tsan --parallel
ctest --preset tsan --output-on-failure
```

TSan is a race detector, not an allocation/deadline or audio-device check.
Do not combine it with ASan/UBSan/LTO. A focused repeated test is appropriate
for an intermittent concurrency regression, preserving the first failure log.
The current prepared realtime guarantees and remaining gaps are documented in
[the execution contract](ENGINE-CONTRACT.md), with [nodeFlow-derived next steps](NODEFLOW-ADOPTION.md).

All generated audio lives in private temporary directories and is cleaned up.
CTest failures return a nonzero exit status; a configuration with no tests is
an error in test presets. Tests have timeouts and support parallel execution.
To focus a run or preserve machine-readable results:

```sh
ctest --preset default -L unit
ctest --preset default -L integration
ctest --preset default -R 'tooling.parity'
ctest --preset default --output-junit build/default/ctest.xml
./build/default/tests/mam_tests --test-case='*automation*'
```

## Adding a regression

Add a focused `TEST_CASE` in `tests/`, then list any new translation unit in
`tests/CMakeLists.txt`. Include the production header before doctest where
practical so missing header dependencies are visible. Generate tiny audio
fixtures locally. Use exact comparisons for unchanged PCM, sample indices
and event order; state numerical tolerances for DSP/conversion comparisons.
Test both successful behavior and the failure that motivated a fix.

Do not replace assertions with the implementation's own calculation. Scheduler
parity alone cannot catch all schedulers changing together; compare a saved
pre-change baseline for DSP migrations. A test pass does not establish that a
sound is musically better. Preserve a separate human listening review for musical choices.

## Remaining boundaries

MAM synthesizes/processes graphs and has a separate native streaming PCM stem
renderer. Live/graph-integrated file-player nodes and a native Draft 2020 JSON
Schema validator remain future work. The optional Python companion provides
explicit offline schema preflight. Native noise ownership is covered by its
[seed contract](RANDOMNESS.md); legacy transport truncation, modulation/reset
semantics and summation order still prevent a universal determinism claim.
The selected Python ST/YM
chip sound is not reproduced by `mam_chip`; retain printed approved buses.
See [DigitalMania integration](NOROCKSTAR.md).
