# MAM offline Python companion

MAM can launch the optional companion without embedding Python in the audio
engine. Commands run offline, exchange JSON/files and never open the audio
device. Python handles preparation and experiments; the native stem renderer
places the resulting PCM on an integer-frame timeline.

## Build and diagnose

Prepare Python 3.13 using [getting started](../docs/GETTING-STARTED.md).
A local `.venv/bin/python` is detected at configure time, or choose explicitly:

```sh
cmake --preset python -DMAM_PYTHON_EXECUTABLE=/absolute/path/to/python
cmake --build --preset python --parallel
ctest --preset python --output-on-failure
./build/python/mam tools doctor
```

Omit `-DMAM_PYTHON_EXECUTABLE=...` to use the local `.venv`.
The normal `default`/`release` builds do not need Python packages. The `python`
preset deliberately fails configuration if test dependencies are absent.
No configure/build/run command installs packages or downloads models.

`MAM_PYTHON` or `mam tools --python PATH ...` overrides the interpreter at
runtime. `MAM_PYTHON_TOOLS_DIR` / `--worker-dir DIR` points to the directory
containing `worker.py` when moving a binary away from its source checkout.
`--timeout SECONDS` defaults to 300. Timeout returns 124; cancellation returns
128 + signal; interpreter startup failure returns 127. Worker failures return
nonzero and machine-readable JSON. The launcher passes an argument vector
directly to `posix_spawnp`; it never invokes a shell. Its process group allows
timeout/cancellation to stop FFmpeg children too.

The worker can also run directly:

```sh
.venv/bin/python -I -B python/worker.py doctor
.venv/bin/python -I -B python/worker.py run path/to/job.json
```

## Supported jobs

| Operation | Behavior | Backend |
| --- | --- | --- |
| `inspect` | Streaming peak/RMS, mono/stereo correlation, silence flags, optional LUFS/true peak | NumPy, SoundFile, FFmpeg EBU R128 |
| `compare` | Aligned PCM difference, exact equality or explicit absolute tolerance, failure diagnostics | NumPy, SoundFile |
| `prepare` | Exact frame window, explicit gain, optional sample-rate conversion | SoundFile, NumPy, soxr |
| `effects` | Stateful block processing, explicit tail length | Pedalboard built-in effects |
| `decompose` | Harmonic/percussive/residual estimates, maximum 60-second window | librosa HPSS |
| `chip` | One monophonic voice from the approved ST/YM-style palette, 44.1 kHz | Extracted `chip_st` math, NumPy/SciPy |
| `validate` | Actual Draft 2020-12 preflight validation with remote references disabled | jsonschema |
| `decode` | First audio stream decoded to Float32 WAV; optional explicit rate | FFmpeg |
| `export` | MP3 copy at an explicit bitrate | FFmpeg/libmp3lame |

Praat/Parselmouth, pyloudnorm and Matplotlib are retained as optional Python
prototype dependencies, not new job operations yet. External VST/AU hosting,
voice resynthesis and realtime Python DSP are not implemented by this bridge.
Doctor reports local capabilities without loading external plugins. Plugin
presence is not proof of architecture or runtime compatibility. No stem-separation
service is called by this worker.

## Example: analysis of a selected window

```json
{
  "schema_version": 1,
  "operation": "inspect",
  "input": {
    "path": "source.wav",
    "start_frame": 44100,
    "frames": 88200
  },
  "output_dir": "analysis-001",
  "params": {"loudness": true}
}
```

Paths resolve relative to the job file, not the shell's working directory.
`output_dir` must be new and its parent must exist. Float32 output preserves
channel count and sample rate unless the job explicitly changes rate. Nothing
normalizes automatically or boosts quiet separation residue. Silent/short
inputs return JSON `null` with a reason for unavailable loudness values.
Input NaN/infinity, invalid windows, unknown properties and duplicate JSON
keys fail. The job schema is `mam_tools/job.schema.json`.

The `effects` chain supports Gain, Compressor, Limiter, HighpassFilter,
LowpassFilter, PeakFilter, HighShelfFilter, LowShelfFilter, Delay, Reverb,
Chorus, Distortion and Bitcrush, with their Pedalboard parameter names.
State persists across blocks; `tail_frames` is explicit, never guessed.
Plugin latency is not automatically compensated. Adapters that return a
different number of frames fail rather than silently shifting the timeline.

HPSS outputs are **estimates, not isolated instrument stems**. The residual
allows reconstruction of the selected input within Float32 precision. Chip
jobs use explicit `start_frame`/`frames` note events; overlapping notes in one
voice fail. The synthesis math is checked against hashes captured from the
production helper, whose source hash is recorded in `tests/chip_golden.json`.
No historical renderer is imported or executed.

## Complete runnable demonstration

The [DigitalMania studies](../examples/digitalmania/README.md) synthesize their
own buses, mix them through native MAM and inspect the outputs through this
companion. From the repository root:

```sh
.venv/bin/python examples/digitalmania/render.py --mam build/python/mam --output build/digitalmania-001
```

Outputs remain in the ignored build directory. Use a new output directory for
each run. The generated job files also provide concrete examples of the contracts.

## Result and cache contract

### Aligned comparisons

`compare` reads both selected windows in bounded Float64 blocks. It never
adjusts gain, shifts audio, resamples or truncates mismatched lengths. Both
inputs must have equal rates, channel counts and selected frame counts:

```json
{
  "schema_version": 1,
  "operation": "compare",
  "input": {"path": "native.wav"},
  "output_dir": "comparison-001",
  "params": {
    "reference": {"path": "python.wav"},
    "max_abs_error": 0
  }
}
```

Both inputs support `start_frame`/`frames`; reference paths also resolve
relative to the job file. The default tolerance is zero. A mismatch returns
nonzero and saves metrics in `failure.json`, without a successful result marker.
Set `fail_on_difference: false` to collect differences without failing the job;
then inspect `within_tolerance` explicitly. Reports distinguish exact decoded
sample equality from tolerance acceptance, include peak/RMS error and locate
the first/maximum difference using zero-based window frames/channels.
Both file hashes participate in cache/provenance. No automatic gain matching
or latency compensation can hide a shifted or changed signal.

### Publication of artifacts

Each successful Python job publishes `result.json` with input SHA256 hashes,
parameters, Python/package/FFmpeg versions, worker-source hashes, output frame
counts/formats and artifact SHA256 hashes. It checks source hashes again before
publishing. Artifacts are written privately then linked with exclusive-create
semantics. A failure may leave a newly reserved output directory with
`failure.json`; cancellation may leave a partial directory. Only a parsed
`result.json` with `status: ok` identifies completion.

The default cache is `mam/build/python-cache`; set `MAM_CACHE_DIR` to choose
another local writable directory. Keys include source content, parameters,
runtime versions and worker implementation. Cache hits verify artifact hashes;
corrupt entries are ignored. Outputs are independent copies, not writable
hardlinks into the cache. Cache-write failure is a warning after a successful
job. Set `"cache": false` for a deliberate uncached comparison. Numba and
Matplotlib cache locations also stay under this directory in the CLI worker.

`mam tools run` does not silently enable the native CLI's `--schema-strict`.
Use the explicit `validate` operation for Python schema preflight. Native rack
strict mode remains unavailable until its own validator is integrated.

## Environment and tests

The optional stack is tested on Python 3.13.18 on macOS arm64.
`pyproject.toml` defines analysis/effects/voice/plots/test groups.
`requirements.lock.txt` snapshots the complete tested optional stack, including
transitive versions. It is a version pin list, not a wheel-hash or universal
cross-platform lock. Keep environments, wheels, caches and generated audio out
of Git; vendor small C++ source dependencies with licenses and provenance.

For a fresh explicit installation (network/package availability required):

```sh
python3.13 -m venv .venv
./.venv/bin/python -m pip install -r python/requirements.lock.txt
cmake --preset python -DMAM_PYTHON_EXECUTABLE="$PWD/.venv/bin/python"
```

The companion and DigitalMania examples add 69 pytest cases to the native suite. Tests use generated
fixtures, private temporary directories and no production/reference media.
They cover contracts, exact cuts, channel order, silence/nonfinite audio,
stateful effects/tails, resampling, HPSS reconstruction, chip fixtures, cache
reuse/corruption, schema failure, shell-free dispatch, timeouts, PCM conversion,
native placement/fades/overlaps and Python→native sample equality. Comparison
tests include offset windows, block boundaries, sub-Float32 differences,
format mismatches, changed reference hashes and nonzero CLI failures.
