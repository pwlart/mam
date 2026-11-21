# DigitalMania synthesis studies

Sound programmed by **norockstar / Matthias Thomas Gabriel Amminger**.
These recipes reuse the mathematical sound palette from DigitalMania in new,
short arrangements. They read no source recordings and download no samples.

| Study | Source and construction | Duration |
| --- | --- | --- |
| Native pulse | E-minor adaptation, C++ kick/acid voice and slow cutoff LFO | 12.25 seconds rendered with the command below |
| Telephone exchange | 425 Hz tone, dual tones, filtered noise, relay envelopes | 32 beats + 2-second tail |
| YM room | Original E-minor phrase, square/pulse timbres, 50 Hz control, LFSR noise and zaps | 64 beats + 2-second tail |
| Sonar dub | Pings, synthesized bubbles, filtered ambience and alternating echoes | 32 beats + 3-second tail |

Tempo is **161.5 BPM**, sample rate **44.1 kHz**. The YM study is an original
software chip texture, not a recording or cycle-accurate hardware emulation.
The native pulse uses different instruments; it is an adaptation, not a
bit-identical C++ port of the Python palette.

## Native-only example

From the repository root, after the [default build](../../docs/GETTING-STARTED.md):

```sh
./build/default/mam --rack examples/digitalmania/pulse.json \
  --wav build/digitalmania-pulse-001.wav --sr 44100 --duration 12 --random-seed 12
```

## Render all three Python studies through native MAM

Prepare the [Python environment](../../docs/GETTING-STARTED.md), then:

```sh
.venv/bin/python examples/digitalmania/render.py \
  --mam build/python/mam --output build/digitalmania-001 --seed 12
open build/digitalmania-001/index.html
```

Use `--example telephone`, `--example ym` or `--example sonar` for one study.
Each output directory must be new. No automatic playback, normalization or
replacement of earlier renders occurs.

Each study writes generated Float32 buses, a native stem manifest, native mix
and result, an analysis job/result, and `demo.json` with events and hashes.
The renderer checks exact frame count, finite samples, headroom and equality
with ordered Float32 addition. `examples.json` and a local player index all
results. Open that player locally: a hosted chat preview cannot read your disk.

## Explore and contribute

Change note choices in `pulse.json`, or edit the arrangement in `render.py`.
The palette in `palette.py` preserves the production helper math; new generator
behavior needs new documented reference evidence. Keep new random streams owned,
state gain/pan/timing explicitly, and keep event tails within the declared output.

The golden hashes/checkpoints in [provenance.json](provenance.json) were captured
from the original pure functions before extraction. Tests use numerical tolerance
across platforms and can verify exact hashes in the recorded runtime. No original
production code or master needs to be available to contributors.

Own code and recipes: [MIT](../../LICENSE). The complete DigitalMania recording,
third-party stems and artwork are separate and not granted by this repository.
