# DigitalMania and MAM

DigitalMania is an art and music project by Matthias Thomas Gabriel Amminger,
AKA norockstar. MAM is developed alongside it as an independent audio tool.

## What is shared here

The [DigitalMania examples](../examples/digitalmania/README.md) extract pure
oscillator, envelope, filtered-noise and chip functions from the author's
production code. They form short new arrangements, not slices of a master WAV.
A native MAM pulse adapts its E-minor material to C++ instruments.

The [provenance manifest](../examples/digitalmania/provenance.json) records the
original source hash, individual function identities, retained material and
reference cases. No private production directory is required to run them.
Random generators are explicit and owned; original function math is retained.

Excluded: all commercial reference stems, recorded drums, voices, wineglass
recordings, mixed tracks, artwork and historical listening deliveries. Fully
synthesized output can be generated locally from the MIT-licensed recipes.

## What MAM does in production

MAM's native offline stem path assembles already prepared PCM buses at exact
frame positions. The Python companion handles preparation, effects, analysis
and experiments through explicit jobs. This does not mean all production DSP
has been ported to C++ or that native `mam_chip` reproduces the Python chip tone.

Use reference cases and aligned comparisons for future ports. Keep accepted
recordings and their original source/build identities separately; a newer engine
must not silently change a historical render.

[www.pwl.art](https://www.pwl.art/) ·
[norockstar on SoundCloud](https://soundcloud.com/norockstar)
