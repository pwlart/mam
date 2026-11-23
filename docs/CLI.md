# CLI guide

All examples assume the repository root and a default build.
Use `./build/default/mam --help` for the complete engine option list.

| Task | Command |
| --- | --- |
| Build identity, JSON, no device | `./build/default/mam --build-info` |
| Native synthesis | `./build/default/mam --rack examples/digitalmania/pulse.json --wav build/pulse-001.wav --sr 44100 --duration 12 --random-seed 12` |
| Frame-accurate PCM assembly | `./build/default/mam stems render path/to/mix.json` |
| Optional Python capability report | `./build/default/mam tools doctor` |
| Optional Python job | `./build/default/mam tools run path/to/job.json` |

A graph render without `--wav` may open the audio device. Start with offline
rendering. The live session path has known startup and callback limitations;
see [the roadmap](ROADMAP.md).

## Offline mixing and jobs

[Native stem manifests](NATIVE-STEMS.md) define sample rate, channels, exact
frames, fades and source windows. Inputs are PCM WAV; compressed audio needs
explicit decoding. File paths in manifests resolve relative to the manifest.
New output directories prevent accidental replacement. Failed work is not
a completed result; require a successful exit and the result manifest.

[Python jobs](../python/README.md) use a separate schema and process. Runtime
selection is explicit:

```sh
./build/default/mam tools --python "$PWD/.venv/bin/python" \
  --worker-dir "$PWD/python" --timeout 300 doctor
```

Native `--schema-strict` currently fails with exit 3; it does not silently
approve input. Python's `validate` operation supplies real Draft 2020-12
preflight. Neither a local job nor a plugin path is an untrusted-code sandbox.

## Reproducibility

Record the recipe, source hashes, seed, sample rate and build information.
For native PCM assembly, ordered Float32 summation is tested. Native chip/clap
noise now uses [owned streams and explicit seed precedence](RANDOMNESS.md),
including session racks. Fractional timing, modulation/reset semantics and
summation order remain broader limits; a seed is not proof of arbitrary graph
equivalence across thread/block counts.
