"""Pure sound generators extracted from DigitalMania's radio/chip production palette.

Only NumPy/SciPy math; no recordings, asset paths or legacy renderer imports.
Random functions receive an owned Generator. Rates/timing match the source.
See provenance.json for source and function identities. These are tutorial
building blocks at 44.1 kHz, not a general-purpose hardware emulator.
"""
import numpy as np
import scipy.signal as ss

SR = 44100
BPM = 161.5
BEAT = 60.0 / BPM

def S(sec):
    return int(sec * SR)


def tone(freq, dur, decay=0.15):
    n = max(8, S(dur))
    t = np.arange(n) / SR
    y = np.sin(2 * np.pi * freq * t) * np.exp(-t / decay)
    r = min(S(0.004), n // 3)
    if r > 1:
        y[:r] *= np.linspace(0, 1, r)
    return y.astype(np.float32)


def band(y, lo, hi):
    return ss.sosfilt(ss.butter(2, [lo, hi], "band", fs=SR, output="sos"), y).astype(np.float32)


def relay_click(rng):
    n = S(0.012 + 0.008 * rng.random())
    t = np.arange(n) / SR
    y = rng.standard_normal(n) * np.exp(-t / 0.0025)
    # armature bounce, a few milliseconds later
    b = S(0.003)
    if b < n:
        y[b:] += 0.45 * rng.standard_normal(n - b) * np.exp(-(t[b:] - t[b]) / 0.0015)
    y = band(y, 1800, 7000)
    return (y / (np.abs(y).max() + 1e-9)).astype(np.float32)


def selector(rng, dur=0.45):
    """EMD / Strowger: a short motor whir, then a settle of clicks."""
    n = S(dur)
    t = np.arange(n) / SR
    whir = rng.standard_normal(n) * np.linspace(0.2, 1, n) * np.exp(-((t - 0.25) ** 2) / 0.02)
    fc = np.linspace(280, 900, n)
    # cheap time-varying feel: two bands crossfaded
    a = band(whir, 250, 700)
    b = band(whir, 600, 1600)
    w = np.linspace(0, 1, n).astype(np.float32)
    y = a * (1 - w) + b * w
    y *= np.exp(-t / 0.28)
    return (y / (np.abs(y).max() + 1e-9)).astype(np.float32)


def dial_tone(dur, freq=425.0):
    n = S(dur)
    t = np.arange(n) / SR
    y = np.sin(2 * np.pi * freq * t)
    env = np.ones(n)
    a, z = S(0.02), S(0.08)
    env[:a] = np.linspace(0, 1, a)
    env[-z:] = np.linspace(1, 0, z)
    return (0.9 * y * env).astype(np.float32)


def mf_stab(pair, dur=0.07):
    n = S(dur)
    t = np.arange(n) / SR
    y = np.sin(2 * np.pi * pair[0] * t) + np.sin(2 * np.pi * pair[1] * t)
    env = np.sin(np.pi * np.clip(t / dur, 0, 1)) ** 2
    return (y * env / 2).astype(np.float32)


def sting_2600(dur=0.16):
    return tone(2600.0, dur, decay=0.09)


def ping():
    n = S(0.11)
    t = np.arange(n) / SR
    f = 3200 * (1 + 0.03 * t / 0.11)
    y = np.sin(2 * np.pi * np.cumsum(f) / SR) * np.exp(-t / 0.028)
    return (y / (np.abs(y).max() + 1e-9)).astype(np.float32)


def hydro(rng, dur):
    n = S(dur)
    y = band(rng.standard_normal(n).astype(np.float32), 200, 1800)
    y *= np.hanning(n).astype(np.float32)
    return (y / (np.abs(y).max() + 1e-9) * 0.35).astype(np.float32)


def bubbles(rng, dur, descending=True, n_bub=26):
    n = S(dur)
    y = np.zeros(n, np.float32)
    u = np.sort(rng.random(n_bub))
    u = u ** 1.55 if descending else 1 - (1 - u) ** 1.5
    for k, uu in enumerate(u):
        f = (780 if descending else 260) + ((240 if descending else 700) - (780 if descending else 260)) * uu
        L = S(0.035 + 0.04 * float(rng.random()))
        t = np.arange(L) / SR
        ph = np.cumsum(f * (1 - 0.25 * t)) / SR
        blip = np.sin(2 * np.pi * ph) * np.exp(-t / 0.018)
        blip += rng.standard_normal(L) * np.exp(-t / 0.012) * 0.35
        at = int(uu * (n - L - 1))
        y[at : at + L] += blip.astype(np.float32)
    y = ss.sosfilt(ss.butter(2, 1800, "low", fs=SR, output="sos"), y)
    peak = np.abs(y).max() + 1e-9
    return (y / peak).astype(np.float32)


def nsq_zap(rng, dur=0.45):
    """GIST-style: the chip cannot sweep, the 68000 does it for the chip."""
    n = S(dur)
    t = np.arange(n) / SR
    f = 2400 * (80 / 2400) ** (t / dur)
    ph = np.cumsum(f) / SR
    sq = np.where((ph % 1) < 0.5, 1.0, -1.0)
    nz = band(rng.standard_normal(n).astype(np.float32), 800, 5000)
    env = np.exp(-t / 0.11)
    y = (0.75 * sq + 0.25 * nz) * env
    return (y / (np.abs(y).max() + 1e-9)).astype(np.float32)


def ay_noise(n, period=8):
    """One shared noise source, the YM's single LFSR, held for `period` samples."""
    m = n // period + 2
    reg = 0x1FFFF
    raw = np.empty(m, np.float32)
    for i in range(m):
        bit = ((reg >> 0) ^ (reg >> 3)) & 1
        reg = ((reg >> 1) | (bit << 16)) & 0x1FFFF
        raw[i] = 1.0 if reg & 1 else -1.0
    return np.repeat(raw, period)[:n]


def ym_section(dur):
    """Three squares and one noise. Arpeggio retargets every PAL frame. Original line in E."""
    n = S(dur)
    frame = int(SR / 50)
    bass_seq = [0, 0, 0, 7, 0, 10, 0, 7]  # E, with a minor 7th and a fifth, in eighths
    lead_seq = [0, 3, 7, 10, 12, 10, 7, 3]
    arp = (0, 7, 12, 7)
    yb = np.zeros(n, np.float64)
    yl = np.zeros(n, np.float64)
    yn = np.zeros(n, np.float64)
    noise = ay_noise(n, period=6)
    pb = pl = 0.0
    eighth = int(BEAT * SR / 2)
    for i0 in range(0, n, frame):
        L = min(frame, n - i0)
        ei = i0 // max(1, eighth)
        fb = 41.203 * 2 ** (bass_seq[ei % 8] / 12)
        fl = 164.814 * 2 ** ((lead_seq[ei % 8] + arp[(i0 // frame) % 4]) / 12)
        tb = (pb + np.arange(L) * fb / SR) % 1
        tl = (pl + np.arange(L) * fl / SR) % 1
        # envelope restarts on each eighth, the arp only chatters the pitch
        pos = (i0 % eighth) / eighth
        env_b = np.exp(-pos * 1.2)
        env_l = np.exp(-pos * 2.4)
        yb[i0 : i0 + L] = np.where(tb < 0.5, 1.0, -1.0) * env_b
        yl[i0 : i0 + L] = np.where(tl < 0.5, 1.0, -1.0) * env_l * 0.7
        # noise hat on the off-eighth
        if ei % 2 == 1:
            yn[i0 : i0 + L] = noise[i0 : i0 + L] * np.exp(-pos * 6.0) * 0.45
        pb = (pb + L * fb / SR) % 1
        pl = (pl + L * fl / SR) % 1
    y = (yb + yl + yn).astype(np.float32)
    y = ss.sosfilt(ss.butter(2, 6500, "low", fs=SR, output="sos"), y)
    # a small speaker box, the way an ST monitor was not a studio monitor
    y = ss.sosfilt(ss.butter(2, 180, "high", fs=SR, output="sos"), y)
    return (y / (np.abs(y).max() + 1e-9)).astype(np.float32)
