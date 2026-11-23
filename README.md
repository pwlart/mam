# MAM — Maximum Audio Madness

**Program sound. Make it move. Keep it reproducible.**

MAM is a C++20 audio playground for macOS: synthesize instruments, route and
modulate a graph, render offline, and assemble PCM stems on an exact sample
timeline. An optional Python companion provides sound experiments and analysis
in a separate process.

**0.1.0 is a community preview**, created by
[norockstar / Matthias Thomas Gabriel Amminger](https://www.pwl.art/).
The offline tools are the best starting point. The live engine is experimental;
its timing, allocation and concurrency limitations are documented.

## Build and hear the first example

Requirements: macOS, Xcode Command Line Tools, CMake 3.21+, Ninja, and a C++20
compiler. C++ dependencies and doctest are vendored. Configure, build and native
tests do not download dependencies or open an audio device.

```sh
cmake --preset default
cmake --build --preset default --parallel
ctest --preset default --output-on-failure

./build/default/mam --rack examples/digitalmania/pulse.json \
  --wav build/digitalmania-pulse-001.wav --sr 44100 --duration 12 --random-seed 12
open build/digitalmania-pulse-001.wav
```

Choose a new output filename on subsequent renders; MAM refuses overwrites.
The native pulse adapts DigitalMania's programmed rhythm and E-minor material
to MAM instruments. It is not a cut from the released recording.

## DigitalMania: sound from code

The collection comes from the mathematical sound generators used in
norockstar's DigitalMania production. No recordings, commercial stems, voices,
game samples or mastered tracks are bundled or downloaded.

| Example | What it teaches | Runtime |
| --- | --- | --- |
| Native pulse | Kick, acid voice, note locks and a slow filter LFO | C++ MAM |
| Telephone exchange | Oscillators, dual tones, filtered noise and relay envelopes | Python synthesis → native MAM mix |
| YM room | Original chip phrase, 50 Hz control, LFSR noise and software zaps | Python synthesis → native MAM mix |
| Sonar dub | Procedural pings, bubbles, filtered ambience and timed echoes | Python synthesis → native MAM mix |

The Python studies retain the production generator math in short new
arrangements. Generated Float32 buses are mixed and checked through MAM.
[Render the studies](examples/digitalmania/README.md) ·
[Source provenance](examples/digitalmania/provenance.json)

## Available now

- Native kick, clap, extended acid and chip instruments; modulation, routing,
  sidechain processing, delay and reverb.
- Offline graph rendering with scheduler parity checks.
- Streaming mono/stereo PCM stem assembly at 44.1 or 48 kHz, integer frame
  placement, explicit fades and ordered mixing.
- Optional Python jobs for inspection, comparison, preparation, effects,
  chip synthesis, schema validation, decoding and export.
- JSON build identity, generated-fixture tests, doctest, and separate
  debug/ASan/UBSan/TSan presets.

MAM is not yet a DAW, a VST/AU host, a Linux/Windows application or a
cycle-accurate chip emulator. Native strict JSON Schema validation is explicitly
unavailable; the optional Python preflight is separate.

## Find your way

| Goal | Start here |
| --- | --- |
| Build, configure and render | [Getting started](docs/GETTING-STARTED.md) |
| Understand commands | [CLI guide](docs/CLI.md) |
| Make a sample-free study | [DigitalMania recipes](examples/digitalmania/README.md) |
| Use Python tools | [Companion guide](python/README.md) |
| Arrange PCM precisely | [Native stem contract](docs/NATIVE-STEMS.md) |
| Understand ownership | [Execution contract](docs/ENGINE-CONTRACT.md) |
| Understand offline cancellation and failure | [Worker lifecycle](docs/OFFLINE-WORKERS.md) |
| Run or add tests | [Testing](docs/TESTING.md) |
| Find a contribution | [Contributing](CONTRIBUTING.md) and [roadmap](docs/ROADMAP.md) |
| Browse the references | [Documentation index](docs/README.md) |

## Community and license

Use [GitHub issues](https://github.com/pwlart/mam/issues) for reproducible bugs
and focused proposals. Improvements to examples, docs, diagnostics and tests
are welcome alongside DSP work. See [governance](GOVERNANCE.md),
[community conduct](CODE_OF_CONDUCT.md) and [security reporting](SECURITY.md).

MAM's own code and original example recipes are **MIT-licensed**.
[Third-party notices](THIRD_PARTY_NOTICES.md) retain upstream licenses.
DigitalMania's full recordings, reference stems and artwork are separate works
and are not included in this repository or this license grant.
