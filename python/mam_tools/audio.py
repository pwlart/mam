"""Bounded audio inspection and explicit frame-window operations."""
import math
import re
import shutil
import subprocess
from contextlib import contextmanager

BLOCK = 65536


def db(value):
    return 20 * math.log10(value) if value > 0 else None


@contextmanager
def source_window(source):
    import soundfile as sf
    with sf.SoundFile(source["path"]) as stream:
        if stream.channels not in (1, 2) or stream.samplerate < 8000:
            raise ValueError("This adapter supports mono/stereo audio at >=8 kHz")
        start = source.get("start_frame", 0)
        frames = source.get("frames", stream.frames - start)
        if frames <= 0 or start > stream.frames or frames > stream.frames - start:
            raise ValueError("Frame window is empty or extends beyond the source")
        stream.seek(start)
        yield stream, frames


def read_checked(stream, count):
    import numpy as np
    audio = stream.read(count, dtype="float32", always_2d=True)
    if len(audio) != count:
        raise ValueError("Unexpected end of audio file")
    if not np.isfinite(audio).all():
        raise ValueError("Audio contains NaN or infinity")
    return audio


def ffmpeg(args):
    executable = shutil.which("ffmpeg")
    if not executable:
        raise RuntimeError("FFmpeg is required for this operation")
    result = subprocess.run([executable, "-hide_banner", "-nostdin", *args],
                            capture_output=True, text=True, check=False)
    if result.returncode:
        raise RuntimeError("FFmpeg failed: " + result.stderr[-3000:])
    return result


def inspect_audio(source, loudness=True):
    import numpy as np
    with source_window(source) as (stream, frames):
        rate, channels = stream.samplerate, stream.channels
        sums = np.zeros(channels)
        squares = np.zeros(channels)
        cross = mono_square = peak = 0.0
        remaining = frames
        while remaining:
            block = read_checked(stream, min(BLOCK, remaining)).astype(np.float64)
            remaining -= len(block)
            peak = max(peak, float(np.max(np.abs(block))))
            sums += block.sum(axis=0)
            squares += (block * block).sum(axis=0)
            mono_square += float(np.square(block.mean(axis=1)).sum())
            if channels == 2:
                cross += float((block[:, 0] * block[:, 1]).sum())
        rms = float(np.sqrt(squares.sum() / (frames * channels)))
        correlation = None
        if channels == 2:
            variance = np.maximum(squares - sums * sums / frames, 0)
            denominator = math.sqrt(float(variance[0] * variance[1]))
            if denominator > 0:
                correlation = float(np.clip((cross - sums[0] * sums[1] / frames) / denominator, -1, 1))
        result = {
            "sample_rate": rate, "channels": channels, "frames": frames,
            "source_start_frame": source.get("start_frame", 0),
            "duration_seconds": frames / rate, "sample_peak_dbfs": db(peak),
            "rms_dbfs": db(rms), "mono_rms_dbfs": db(math.sqrt(mono_square / frames)),
            "stereo_correlation": correlation, "finite": True, "silent": peak == 0,
            "near_silent": peak > 0 and peak < 0.001 and rms < 0.0001,
            "integrated_lufs": None, "true_peak_dbtp": None,
        }
    if loudness and peak > 0 and frames >= math.ceil(rate * 0.4):
        start = source.get("start_frame", 0)
        chain = f"atrim=start_sample={start}:end_sample={start + frames},ebur128=peak=true"
        process = ffmpeg(["-i", source["path"], "-map", "0:a:0", "-af", chain, "-f", "null", "-"])
        summary = process.stderr.rsplit("Summary:", 1)[-1]
        for label, pattern in [("integrated_lufs", r"I:\s*([-+\d.inf]+)\s+LUFS"),
                               ("true_peak_dbtp", r"Peak:\s*([-+\d.inf]+)\s+dBFS")]:
            match = re.search(pattern, summary)
            if not match:
                raise ValueError("FFmpeg loudness summary is missing " + label)
            value = float(match.group(1))
            result[label] = value if math.isfinite(value) else None
        result["loudness_backend"] = "ffmpeg ebur128=peak=true"
    else:
        result["loudness_unavailable_reason"] = "disabled" if not loudness else "silent_or_shorter_than_400ms"
    return result


def prepare(source, params, destination):
    import numpy as np
    import soundfile as sf
    with source_window(source) as (stream, frames):
        rate = params.get("sample_rate", stream.samplerate)
        gain = np.float32(10 ** (params.get("gain_db", 0) / 20))
        converter = None
        if rate != stream.samplerate:
            import soxr
            converter = soxr.ResampleStream(stream.samplerate, rate, stream.channels, dtype="float32", quality="HQ")
        with sf.SoundFile(destination, mode="x", samplerate=rate, channels=stream.channels, format="WAV", subtype="FLOAT") as output:
            remaining = frames
            while remaining:
                data = read_checked(stream, min(BLOCK, remaining))
                remaining -= len(data)
                if converter:
                    data = converter.resample_chunk(data, last=remaining == 0)
                data = data * gain
                if not np.isfinite(data).all(): raise ValueError("Nonfinite prepared audio")
                output.write(data)
    return {"gain_db": params.get("gain_db", 0), "resampled": converter is not None,
            "normalization": "none", "frame_origin": "selected source start"}


def effects(source, params, destination):
    import numpy as np
    import pedalboard
    import soundfile as sf
    chain = pedalboard.Pedalboard([
        getattr(pedalboard, item["type"])(**item.get("params", {})) for item in params["chain"]
    ])
    with source_window(source) as (stream, frames):
        total = frames + params.get("tail_frames", 0)
        position = 0
        # Fixed size avoids ambiguity in Pedalboard's channel-layout detection
        # when a final block contains fewer frames than channels.
        with sf.SoundFile(destination, mode="x", samplerate=stream.samplerate, channels=stream.channels, format="WAV", subtype="FLOAT") as output:
            while position < total:
                count = min(8192, total - position)
                block = np.zeros((stream.channels, 8192), np.float32)
                incoming = min(count, max(0, frames - position))
                if incoming: block[:, :incoming] = read_checked(stream, incoming).T
                processed = chain(block, stream.samplerate, buffer_size=8192, reset=position == 0)
                if processed.shape != block.shape:
                    raise ValueError("Effect buffers frames; this adapter requires fixed-length output")
                if not np.isfinite(processed).all(): raise ValueError("Nonfinite effect output")
                output.write(processed[:, :count].T)
                position += count
    return {"tail_frames": params.get("tail_frames", 0), "latency_compensated": False,
            "normalization": "none", "chain": params["chain"]}


def decompose(source, params, directory):
    import librosa
    import numpy as np
    import soundfile as sf
    with source_window(source) as (stream, frames):
        if frames > stream.samplerate * 60:
            raise ValueError("HPSS jobs are limited to 60 seconds; select a frame window")
        data = read_checked(stream, frames)
        rate = stream.samplerate
    harmonic, percussive = librosa.effects.hpss(data.T, margin=params.get("margin", 1.0))
    outputs = {"harmonic.wav": harmonic.T, "percussive.wav": percussive.T,
               "residual.wav": data - harmonic.T - percussive.T}
    for name, samples in outputs.items():
        if samples.shape != data.shape or not np.isfinite(samples).all():
            raise ValueError("Invalid HPSS output shape or samples")
        with sf.SoundFile(directory / name, "x", samplerate=rate, channels=data.shape[1], format="WAV", subtype="FLOAT") as output:
            output.write(samples)
    return {"method": "librosa HPSS", "interpretation": "harmonic/percussive estimates, not isolated instrument stems",
            "reconstruction": "harmonic + percussive + residual", "normalization": "none"}
