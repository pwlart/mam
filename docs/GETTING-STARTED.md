# Getting started

MAM 0.1.0 targets macOS and Apple Silicon development. It requires Xcode Command
Line Tools, a C++20 compiler, CMake 3.21+, Ninja and Git. Linux and Windows are
not supported by the current CoreAudio application.

If needed, install the tools explicitly with your package manager. With Homebrew:

```sh
brew install cmake ninja
```

From the repository root:

```sh
cmake --preset default
cmake --build --preset default --parallel
ctest --preset default --output-on-failure
./build/default/mam --build-info
./build/default/mam --rack examples/digitalmania/pulse.json \
  --wav build/digitalmania-pulse-001.wav --sr 44100 --duration 12 --random-seed 12
```

Use a new output filename each time. Open the WAV in your preferred local
player at a comfortable level. Native build and test commands do not download
dependencies or require an audio device. The executable is not installed globally.

## Optional Python synthesis and analysis

Use Python 3.13 in a dedicated environment. Dependency installation is a separate
explicit step; building MAM never runs pip.

```sh
python3.13 -m venv .venv
.venv/bin/python -m pip install -r python/requirements.lock.txt
cmake --preset python -DMAM_PYTHON_EXECUTABLE="$PWD/.venv/bin/python"
cmake --build --preset python --parallel
ctest --preset python --output-on-failure
./build/python/mam tools doctor
.venv/bin/python examples/digitalmania/render.py \
  --mam build/python/mam --output build/digitalmania-001
open build/digitalmania-001/index.html
```

FFmpeg is needed for codec operations and loudness/true-peak jobs; the supplied
DigitalMania renderer uses sample-peak inspection and does not need it.
[Companion details](../python/README.md) explain capabilities and job schemas.

## Layout

- `src/`: C++ engine, CLI, platform adapters and offline tools.
- `python/`: optional worker, operation schemas and Python tests.
- `examples/digitalmania/`: runnable synthesis recipes and provenance.
- `tests/`: generated-fixture tests; `tests/fixtures/` holds a regression rack.
- `vendor/`: unmodified, pinned dependencies with licenses and hashes.
- `docs/`: contracts, references and contribution roadmap.
- `build/`: ignored local builds, reports and renders.

A copied executable retains its configured Python worker path. Set
`MAM_PYTHON_TOOLS_DIR` and `MAM_PYTHON`, or use the explicit CLI overrides
when moving it. A build's identity belongs to that build; rebuild after committing.
