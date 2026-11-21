# Native offline stem renderer

`mam stems render JOB.json` streams PCM through the C++20 reader/writer. It is
an offline arrangement tool, separate from the legacy synth rack/transport
engine. No Python is required; no disk read runs in an audio callback. Python
effects/preparation outputs can be used directly as stem inputs.

```json
{
  "schema_version": 1,
  "sample_rate": 44100,
  "channels": 2,
  "frames": 132300,
  "output_dir": "native-pass-001",
  "stems": [
    {
      "path": "prepared.wav",
      "source_start_frame": 44100,
      "destination_start_frame": 22050,
      "frames": 88200,
      "gain_db": 0,
      "fade_in_frames": 441,
      "fade_out_frames": 2205
    }
  ]
}
```

All paths resolve relative to the JSON file. Both the selected source range
and destination range must fit exactly; no implicit truncation occurs. Default
source/destination offsets are zero, gain is 0 dB and fades are disabled.
`frames` is required for the output and each stem. Optional `block_frames`
defaults to 4096 and accepts 1..65536. Different block sizes produce identical
samples for the same manifest and input on the tested toolchain.

Only mono/stereo at 44.1 or 48 kHz is currently accepted for a render. All
inputs must match its rate/channels; prepare other material explicitly. The
reader accepts CoreAudio-readable linear PCM, including Float32 and PCM16/24
WAV. It does not decode compressed recordings in this path. Negative/fractional
frame values, out-of-range windows, duplicate keys and unknown fields fail.

Fades use half-cosine curves with exact zero/unity endpoints; lengths are zero
or at least two frames and cannot overlap within a stem. Gain is bounded to
−120..+24 dB. Stems sum in manifest order, with no limiter, clipping or automatic
normalization. Floating output may exceed unity; inspect the result before
encoding/delivery. Silence fills unused timeline regions.

Outputs are a new directory containing `audio.wav` (Float32) and `result.json`.
The manifest records the recipe, input-file SHA1 identities, output sample
SHA1, peak and dimensions. These hashes are provenance/comparison identifiers,
not a security protocol. Source identities are rechecked before success. Input
NaN/infinity and nonfinite mix output fail. An incomplete render can leave a
partial WAV but no successful result manifest; use a new output directory for
a retry. Existing output directories/files are never reused.

New result manifests also include `build`: the same CMake version, Git revision/
dirty state, compiler, build type and instrumentation reported by `mam --build-info`.
It describes the executable's last build, not the checkout at render time.
Continue storing binary hashes and source identities with production evidence.

Input and output I/O uses bounded blocks; the full set is not held in memory.
RIFF WAV size is limited below 4 GiB with a header allowance. Larger output,
RF64/CAF streaming, live file-player nodes, tempo maps and graph-integrated
file input remain future work. This renderer's integer-frame clock bypasses
the legacy fractional-beat truncation; it does not change that synth transport.

See [Python companion](../python/README.md) for the complete chip→effects→native
demo and [testing](TESTING.md) for native/Python regression commands.
