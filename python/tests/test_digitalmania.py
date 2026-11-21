"""Public synthesis examples need neither the original production tree nor media."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys

import numpy as np
import pytest
import soundfile as sf

EXAMPLES = Path(__file__).resolve().parents[2] / "examples" / "digitalmania"
sys.path.insert(0, str(EXAMPLES))
spec = importlib.util.spec_from_file_location("digitalmania_render", EXAMPLES / "render.py")
render = importlib.util.module_from_spec(spec)
spec.loader.exec_module(render)
palette = render.palette
REFERENCE = json.loads((EXAMPLES / "provenance.json").read_text())["golden_reference"]
RANDOM = {"relay_click", "selector", "hydro", "bubbles", "nsq_zap"}


@pytest.mark.parametrize("case", REFERENCE["cases"], ids=lambda c: c["function"])
def test_original_production_reference(case):
    args = case["args"]
    if case["function"] in RANDOM:
        args = [np.random.default_rng(case["seed"]), *args]
    actual = getattr(palette, case["function"])(*args, **case["kwargs"])
    assert actual.dtype == np.float32
    assert len(actual) == case["frames"]
    assert np.isfinite(actual).all()
    np.testing.assert_allclose(actual[case["checkpoint_frames"]], case["checkpoint_values"],
                               atol=2e-6, rtol=2e-5)
    assert np.max(np.abs(actual)) == pytest.approx(case["peak"], abs=2e-6, rel=2e-5)
    assert np.sqrt(np.mean(actual.astype(np.float64) ** 2)) == pytest.approx(
        case["rms"], abs=2e-6, rel=2e-5)
    if os.environ.get("MAM_EXACT_DIGITALMANIA_REFERENCE") == "1":
        assert hashlib.sha256(actual.astype("<f4").tobytes()).hexdigest() == case["sha256_float32_le"]


@pytest.mark.parametrize("name", render.SCENES)
def test_seeded_scenes_are_bounded_and_repeatable(name):
    first, events = render.generate(name, 12)
    repeat, repeat_events = render.generate(name, 12)
    changed, _ = render.generate(name, 13)
    assert events == repeat_events
    count = render.frame(render.SCENES[name][1]) + render.SCENES[name][2] * palette.SR
    total = np.zeros((count, 2), np.float32)
    for label, samples in first.items():
        assert samples.shape == (count, 2)
        assert np.isfinite(samples).all()
        np.testing.assert_array_equal(samples, repeat[label])
        total += samples
    assert any(not np.array_equal(first[key], changed[key]) for key in first)
    assert .001 < np.max(np.abs(total)) < .8
    assert np.count_nonzero(total[:1000]) > 0
    assert np.max(np.abs(total[-100:])) == 0
    assert all(0 <= event["start_frame"] < event["start_frame"] + event["frames"] <= count
               for event in events)


def test_full_native_pipeline_and_overwrite_refusal(binary, tmp_path):
    output = tmp_path / "studies with spaces"
    command = [sys.executable, str(EXAMPLES / "render.py"), "--mam", binary,
               "--output", str(output)]
    result = subprocess.run(command, capture_output=True, text=True, timeout=180)
    assert result.returncode == 0, result.stdout + result.stderr
    manifest = json.loads((output / "examples.json").read_text())
    assert len(manifest) == 3
    for item in manifest:
        directory = output / item["example"]
        audio = directory / item["audio"]
        data, rate = sf.read(audio, dtype="float32", always_2d=True)
        assert data.shape == (item["frames"], 2) and rate == 44100
        assert item["input_recordings"] == [] and item["native_mix_exact"]
        assert hashlib.sha256(audio.read_bytes()).hexdigest() == item["audio_sha256"]
        job = json.loads((directory / "mix.json").read_text())
        assert all((directory / stem["path"]).resolve().is_relative_to(directory / "generated")
                   for stem in job["stems"])
        assert (directory / item["native_result"]).is_file()
        assert (directory / item["analysis_result"]).is_file()
    before = (output / "examples.json").read_bytes()
    again = subprocess.run(command, capture_output=True, text=True, timeout=20)
    assert again.returncode != 0 and "already exists" in again.stderr
    assert before == (output / "examples.json").read_bytes()


def test_native_pulse_is_audible_finite_and_repeatable(binary, tmp_path):
    paths = [tmp_path / f"pulse-{i}.wav" for i in range(2)]
    results = []
    for path in paths:
        process = subprocess.run([binary, "--rack", str(EXAMPLES / "pulse.json"),
            "--wav", str(path), "--sr", "44100", "--duration", "12", "--random-seed", "12"],
            capture_output=True, text=True, timeout=30)
        assert process.returncode == 0, process.stderr
        data, rate = sf.read(path, dtype="float32", always_2d=True)
        assert rate == 44100 and data.shape[1] == 2
        assert data.shape[0] >= 12 * rate
        assert np.isfinite(data).all()
        assert .001 < np.max(np.abs(data)) < .95
        results.append(data)
    np.testing.assert_array_equal(*results)
