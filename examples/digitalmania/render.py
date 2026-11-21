#!/usr/bin/env python3
"""Generate DigitalMania studies from math, then mix them through native MAM.

No input recordings, production paths, downloads or historical renderer imports.
Requires the optional Python synthesis stack. Existing output directories fail.
"""
from __future__ import annotations

import argparse
import hashlib
import html
import json
from pathlib import Path
import subprocess
import sys

import numpy as np
import soundfile as sf

import palette

ROOT = Path(__file__).resolve().parents[2]
SR = palette.SR
BEAT = palette.BEAT
SCENES = {
    'telephone': ('Telephone exchange', 32, 2),
    'ym': ('YM room', 64, 2),
    'sonar': ('Sonar dub', 32, 3),
}


def frame(beat: float) -> int:
    return round(beat * BEAT * SR)


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def edge_fade(samples, attack=.005, release=.04):
    y = np.asarray(samples, np.float32).copy()
    a, b = min(round(attack * SR), len(y) // 2), min(round(release * SR), len(y) // 2)
    if a > 1:
        y[:a] *= np.linspace(0, 1, a, dtype=np.float32)
    if b > 1:
        y[-b:] *= np.linspace(1, 0, b, dtype=np.float32)
    return y


def generate(name: str, seed: int = 12):
    """Return fresh stereo buses and exact frame placements for one study."""
    _, beats, tail = SCENES[name]
    count = frame(beats) + tail * SR
    buses = {label: np.zeros((count, 2), np.float32) for label in ('body', 'detail', 'echo')}
    events = []
    rng = np.random.default_rng(seed)

    def put(bus, samples, at_beat, gain, pan=0., label=''):
        mono = edge_fade(samples)
        start = frame(at_beat)
        if start < 0 or start + len(mono) > count:
            raise ValueError('A generated event exceeds the declared timeline')
        stereo = np.stack([mono * (1 - max(0., pan)), mono * (1 + min(0., pan))], axis=1)
        buses[bus][start:start + len(mono)] += stereo * np.float32(gain)
        events.append({'bus': bus, 'generator': label, 'start_frame': start,
                       'frames': len(mono), 'gain': gain, 'pan': pan})

    if name == 'telephone':
        put('body', palette.dial_tone(2 * BEAT), 0, .12, label='dial_tone')
        for beat in (4, 12, 20, 28):
            put('body', palette.selector(rng), beat, .12, label='selector')
            for offset in (0., .5, 1.25, 2.):
                put('detail', palette.relay_click(rng), beat + offset, .055,
                    pan=.25 if offset % 1 else -.25, label='relay_click')
        # Musical colours only; this is not an operational telephone sequence.
        for beat, pair in ((8, (700, 900)), (16, (900, 1100)), (24, (700, 1300))):
            phrase = palette.mf_stab(pair, .14)
            put('detail', phrase, beat, .12, label='mf_stab')
            for repeat in range(1, 4):
                put('echo', phrase, beat + repeat * .75, .035 * .5 ** (repeat - 1),
                    pan=.4 * (-1) ** repeat, label='mf_stab_echo')
        put('detail', palette.sting_2600(), 30, .035, label='sting_2600')
    elif name == 'ym':
        # The original production helper's E-minor phrase, not a game recording.
        mono = palette.ym_section(64 * BEAT)
        time = np.arange(len(mono)) / SR
        movement = .84 + .16 * np.sin(2 * np.pi * time / (32 * BEAT))
        put('body', mono * movement, 0, .20, label='ym_section')
        for beat in (15.5, 31.5, 47.5, 62.5):
            zap = palette.band(palette.nsq_zap(rng, .16), 450, 2600)
            put('detail', zap, beat, .065, label='nsq_zap')
            put('echo', zap, beat + .75, .025, pan=.35, label='nsq_zap_echo')
    else:
        put('body', palette.hydro(rng, beats * BEAT), 0, .22, label='hydro')
        for index, beat in enumerate((0, 4, 8, 12, 16, 20, 24, 28)):
            ping = palette.ping()
            put('detail', ping, beat, .085, pan=.15 * (-1) ** index, label='ping')
            for repeat in range(1, 5):
                put('echo', palette.band(ping, 900, 4200), beat + .75 * repeat,
                    .035 * .5 ** (repeat - 1), pan=.5 * (-1) ** repeat, label='ping_echo')
        for beat in (6, 14, 22):
            put('detail', palette.bubbles(rng, 1.25, n_bub=9), beat, .075, label='bubbles')
    for bus in buses.values():
        if not np.isfinite(bus).all():
            raise ValueError('Non-finite synthesized audio')
    return buses, events


def render_one(name: str, directory: Path, binary: Path, seed: int):
    directory.mkdir()
    generated = directory / 'generated'
    generated.mkdir()
    buses, events = generate(name, seed)
    count = len(next(iter(buses.values())))
    for label, samples in buses.items():
        with sf.SoundFile(generated / f'{label}.wav', 'x', samplerate=SR,
                          channels=2, subtype='FLOAT', format='WAV') as stream:
            stream.write(samples)
    recipe = {'schema_version': 1, 'sample_rate': SR, 'channels': 2, 'frames': count,
              'output_dir': 'native', 'stems': [
                  {'path': f'generated/{label}.wav', 'frames': count} for label in buses]}
    job = directory / 'mix.json'
    job.write_text(json.dumps(recipe, indent=2) + '\n')
    subprocess.run([str(binary), 'stems', 'render', str(job)], check=True,
                   capture_output=True, text=True, timeout=120)
    output = directory / 'native/audio.wav'
    actual, rate = sf.read(output, dtype='float32', always_2d=True)
    expected = np.zeros_like(actual)
    for samples in buses.values():
        expected += samples
    if rate != SR or actual.shape != (count, 2) or not np.array_equal(actual, expected):
        raise RuntimeError('Native MAM mix differs from the ordered Float32 reference')
    peak = float(np.max(np.abs(actual)))
    if not np.isfinite(actual).all() or peak >= .8:
        raise RuntimeError('Inspect the example headroom before listening')
    # Explicit interpreter/worker selection also supports a binary built elsewhere.
    inspect = directory / 'inspect.json'
    inspect.write_text(json.dumps({'schema_version': 1, 'operation': 'inspect',
        'input': {'path': 'native/audio.wav'}, 'output_dir': 'analysis',
        'cache': False, 'params': {'loudness': False}}, indent=2) + '\n')
    subprocess.run([str(binary), 'tools', '--python', sys.executable,
                    '--worker-dir', str(ROOT / 'python'), 'run', str(inspect)],
                   check=True, capture_output=True, text=True, timeout=120)
    result = {'example': name, 'title': SCENES[name][0], 'sample_rate': SR,
              'channels': 2, 'frames': count, 'duration_seconds': count / SR,
              'peak_dbfs': float(20 * np.log10(peak)), 'seed': seed,
              'input_recordings': [], 'synthesis_source': 'DigitalMania production palette',
              'native_mix_exact': True, 'events': events, 'audio': 'native/audio.wav',
              'audio_sha256': sha(output), 'palette_sha256': sha(Path(__file__).with_name('palette.py')),
              'native_result': 'native/result.json', 'analysis_result': 'analysis/result.json'}
    (directory / 'demo.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--example', choices=['all', *SCENES], default='all')
    parser.add_argument('--output', type=Path, required=True, help='New output directory; refuses overwrite')
    parser.add_argument('--mam', type=Path, default=ROOT / 'build/default/mam')
    parser.add_argument('--seed', type=int, default=12)
    args = parser.parse_args()
    binary = args.mam.resolve()
    if not binary.is_file():
        parser.error('Build MAM first or pass --mam /path/to/mam')
    output = args.output.resolve()
    if output.exists():
        parser.error('Output already exists; choose a new directory')
    output.mkdir(parents=True)
    names = list(SCENES) if args.example == 'all' else [args.example]
    results = [render_one(name, output / name, binary, args.seed) for name in names]
    cards = ''.join(f'<section><h2>{html.escape(item["title"])}</h2>'
                    f'<p>{item["duration_seconds"]:.2f} s · {item["peak_dbfs"]:.1f} dBFS sample peak</p>'
                    f'<audio controls preload="none" src="{item["example"]}/native/audio.wav"></audio></section>'
                    for item in results)
    (output / 'index.html').write_text('<!doctype html><html lang="en"><meta charset="utf-8">'
        '<meta name="viewport" content="width=device-width,initial-scale=1">'
        '<title>DigitalMania — MAM synthesis studies</title>'
        '<style>body{max-width:820px;margin:50px auto;padding:0 24px;font:18px/1.6 system-ui;'
        'background:#17191e;color:#faf9f6}section{padding:20px 0;border-top:1px solid #59606b}'
        'audio{width:100%}h1{color:#edb1d4}</style><h1>DigitalMania / MAM</h1>'
        '<p>Programmed sound by norockstar. Every sound is synthesized; no source recordings.</p>'
        + cards + '</html>')
    (output / 'examples.json').write_text(json.dumps(results, indent=2) + '\n')
    print(json.dumps({'status': 'ok', 'examples': names, 'player': str(output / 'index.html')}))


if __name__ == '__main__':
    main()
