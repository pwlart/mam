# Native noise and reproducible seeds

The native `mam_chip` and `clap` instruments own their random state. Processing
one node does not consume another node's stream. No platform `rand()`, shared
engine or lazy entropy source runs in their processing paths.

## Choosing a seed

```sh
./build/default/mam --rack examples/digitalmania/pulse.json \
  --wav build/pulse-seed-12.wav --random-seed 12
```

`randomSeed` in a rack JSON file accepts integer tokens from 0 through
4,294,967,295. The CLI accepts the same range in decimal, without signs,
whitespace, fractions or trailing characters. Invalid values fail instead of
being silently truncated or wrapped. This field validation does not provide
general native JSON Schema validation.

| Input | Result |
| --- | --- |
| Nonzero `--random-seed` | Overrides each graph/session rack's JSON seed |
| CLI seed omitted or zero | Uses each rack's JSON `randomSeed` |
| Effective root seed zero or absent | Each instrument uses its fixed default stream |
| Nonzero effective seed | Derives a stream from root seed, rack scope and local node ID |

Single graphs have an empty rack scope. Sessions use the rack ID as scope;
offline and live paths use the same local node ID, even though live routing
prefixes it with `rack:`. Moving files or inserting an unrelated node does not
reseed an existing node. Renaming a rack or node intentionally changes its
derived stream. Derivation has a finite 32-bit state space and does not promise
collision-free IDs. Seed zero preserves legacy clap defaults; it does not
decorrelate otherwise identical unseeded voices. Use a nonzero root for that.

The optional Python recipes retain their separate NumPy generators and seeds.
The native CLI seed does not configure Python jobs or existing printed audio.

## Lifecycle and algorithm

`Graph::setRandomSeed(root, scope)` is a stopped-engine configuration operation.
It seeds existing nodes and invalidates realtime preparation; later added nodes
inherit it. `addNode(id, node, localRandomId)` can separate routing identity from
random identity. A standalone `Node::setRandomSeed` receives an already derived
seed. It restarts RNG state, not the rest of the instrument's DSP or parameters.

`reset()` rewinds both noise streams to their configured initial state.
Triggers do not rewind them. Chip preparation also rewinds noise, while clap
preparation retains its legacy state until reset. Production render entry
points prepare and then reset. Existing session offline loop assembly resets
each rack between repeated renders, so chip noise now repeats there; a live
continuous performance keeps advancing. This change does not unify those loop
or modulation-reset semantics.

The versioned derivation contract is `mam-node-seed-v1`: FNV-1a32 over that ASCII
domain, root as LE32, scope byte length as LE64 and its bytes, then local ID byte
length as LE64 and its bytes. A fixed fmix32 avalanche finishes the hash. The
zero result maps to `0x12345678`. No locale, path, `std::hash`, insertion order
or thread number enters the derivation. Zero root bypasses derivation.

`NoiseRandom` uses xorshift32 shifts 13/17/5. Seed zero selects default
`0x12345678`, avoiding the absorbing all-zero state. Chip takes the high 24
bits and maps them to `[-1, 1)` using `value * 2^-23 - 1`. It consumes one draw
per frame, including idle frames and frames where noise mix is zero. Clap draws
only while its envelope is active and retains its original conversion.

## Compatibility and limits

Before this change, `--random-seed` seeded an unused `mt19937`; it affected
neither audible native noise generator. Chip used global C `rand()` and clap
always reset to a fixed seed. This is an explicit behavioral correction:

- Chip noise has a new waveform, including when the root seed is unspecified.
  Unrelated rendering no longer changes its sequence, and reset can replay it.
- Explicit nonzero graph/rack seeds now change clap noise. With no effective
  seed, its original xorshift sequence and sample conversion remain exact.
- Non-noisy synthesis and chip oscillator output at noise mix zero are preserved.
- Clap's legacy conversion actually covers approximately `[-1, 0)`, despite its
  former bipolar comment. Its DC/range correction is a separate sound change.

Use historical builds or approved printed audio to reproduce old noisy output.
This migration does not replace any accepted music master or re-render it.

Known integer vectors, native parsing tests, noisy multi-node fixtures,
serial/parallel and block-partition comparisons exercise this contract.
RNG isolation does not establish universal graph determinism: mutable graph
access, invalid offline cycles, modulation/reset differences, event timing,
floating-point summation order and platform math remain separate concerns.
Audio comparisons use the same build environment and explicitly fixed events.
