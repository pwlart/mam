"""Frame-addressed extraction of norockstar's approved pipeline/chip_st.py palette.

The 44.1 kHz / 50 Hz controls and pulse-edge/envelope math are retained.
No legacy renderer, production path or global random generator is imported.
"""
import numpy as np
import scipy.signal as ss

SR = 44100
FRAME = SR // 50


def blep(phase, step):
    out = np.zeros_like(phase)
    first = phase < step
    t = phase[first] / step[first]
    out[first] = 2 * t - t * t - 1
    last = phase > 1 - step
    t = (phase[last] - 1) / step[last]
    out[last] = t * t + 2 * t + 1
    return out


def square_note(midi, frames, *, arp=(), velocity=.8, tone="bass"):
    n = int(frames)
    t = np.arange(n) / SR
    controls = np.arange(n) // FRAME
    pitch = np.full(n, float(midi))
    if arp:
        pitch += np.asarray(arp)[controls % len(arp)]
    if tone == "lead":
        pitch += .07 * np.sin(2 * np.pi * 5.1 * (controls * FRAME / SR))
    frequency = 440 * 2 ** ((pitch - 69) / 12)
    period = np.clip(np.rint(2_000_000 / (16 * frequency)), 1, 4095)
    step = (2_000_000 / (16 * period)) / SR
    phase = np.mod(np.cumsum(step) - step, 1)
    wave = np.where(phase < .5, 1., -1.)
    wave += blep(phase, step) - blep(np.mod(phase + .5, 1), step)
    attack = .004 if tone != "bass" else .006
    sustain = {"bass": .58, "comp": .20, "lead": .38}[tone]
    decay = {"bass": .12, "comp": .055, "lead": .11}[tone]
    envelope = np.minimum(1., t / attack) * (sustain + (1 - sustain) * np.exp(-t / decay))
    release = min(.045, n / SR * .28)
    envelope *= np.minimum(1., np.maximum(0., (n / SR - t) / release))
    values = np.r_[0., 10 ** (np.linspace(-36., 0., 15) / 20)]
    control_env = envelope[np.minimum(controls * FRAME, n - 1)]
    quantized = values[np.argmin(abs(values[:, None] - control_env[None, :]), axis=0)]
    alpha = np.exp(-1 / (.0015 * SR))
    quantized = ss.lfilter([1 - alpha], [1, -alpha], quantized)
    edge = np.minimum(1., t / .002) * np.minimum(1., np.maximum(0., (n / SR - t) / .004))
    return (wave * quantized * edge * velocity).astype(np.float32)


def render(params, destination):
    import soundfile as sf
    frames, tone = params["frames"], params["tone"]
    output = np.zeros(frames, np.float32)
    previous_end = 0
    for event in sorted(params["events"], key=lambda item: item["start_frame"]):
        start, count = event["start_frame"], event["frames"]
        if start < previous_end or start + count > frames:
            raise ValueError("Chip events must fit the output and may not overlap within a voice")
        output[start:start + count] += square_note(event["midi"], count, arp=event.get("arp", ()),
                                                  velocity=event.get("velocity", .8), tone=tone)
        previous_end = start + count
    low, high = {"bass": (28, 850), "comp": (200, 3600), "lead": (320, 4600)}[tone]
    output = ss.sosfiltfilt(ss.butter(3, [low, high], btype="bandpass", fs=SR, output="sos"), output)
    stereo = np.repeat(output.astype(np.float32)[:, None], 2, axis=1)
    with sf.SoundFile(destination, "x", samplerate=SR, channels=2, format="WAV", subtype="FLOAT") as stream:
        stream.write(stereo)
    return {"palette": "norockstar chip_st", "control_rate_hz": 50,
            "normalization": "none", "cycle_accurate_emulator": False}
