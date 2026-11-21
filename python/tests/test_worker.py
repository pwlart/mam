import hashlib
import json
from pathlib import Path
import shutil

import numpy as np
import pytest
import soundfile as sf

from mam_tools.contracts import read_json
from mam_tools.engine import run_job


def job_file(tmp_path, operation, input_path=None, params=None, output="result", **extra):
    job = {"schema_version": 1, "operation": operation, "output_dir": output, **extra}
    if input_path is not None: job["input"] = {"path": str(input_path)}
    if params is not None: job["params"] = params
    path = tmp_path / (output + ".json")
    path.write_text(json.dumps(job))
    return path


def test_prepare_preserves_exact_frame_window_and_source(audio_file, tmp_path):
    source, original, rate = audio_file
    before = hashlib.sha256(source.read_bytes()).hexdigest()
    job = job_file(tmp_path, "prepare", source)
    request = read_json(job)
    request["input"].update(start_frame=8191, frames=8193)
    job.write_text(json.dumps(request))
    result = run_job(job)
    actual, sample_rate = sf.read(tmp_path / "result/audio.wav", dtype="float32", always_2d=True)
    assert np.array_equal(actual, original[8191:16384])
    assert sample_rate == rate
    assert before == hashlib.sha256(source.read_bytes()).hexdigest()
    assert result["result"]["artifacts"]["audio.wav"]["frames"] == 8193


def test_inspect_loudness_and_antiphase(tmp_path):
    rate = 44100
    wave = .1 * np.sin(2 * np.pi * 440 * np.arange(rate) / rate)
    path = tmp_path / "opposite.wav"
    sf.write(path, np.stack([wave, -wave], axis=1), rate, subtype="FLOAT")
    report = run_job(job_file(tmp_path, "inspect", path))["result"]["details"]
    assert report["frames"] == rate
    assert report["sample_peak_dbfs"] == pytest.approx(-20, abs=.01)
    assert report["stereo_correlation"] == pytest.approx(-1)
    assert report["mono_rms_dbfs"] is None
    assert -25 < report["integrated_lufs"] < -15
    assert report["true_peak_dbtp"] == pytest.approx(-20, abs=.2)


def test_silence_is_reported_without_gain_or_nonfinite_json(tmp_path):
    path = tmp_path / "silence.wav"
    sf.write(path, np.zeros((100, 2), np.float32), 44100, subtype="FLOAT")
    result = run_job(job_file(tmp_path, "inspect", path))
    report = result["result"]["details"]
    assert report["silent"] and not report["near_silent"]
    assert report["integrated_lufs"] is None
    assert report["sample_peak_dbfs"] is None
    json.dumps(result, allow_nan=False)


def test_existing_output_is_never_overwritten(audio_file, tmp_path):
    source, _, _ = audio_file
    output = tmp_path / "result"
    output.mkdir()
    protected = output / "audio.wav"
    protected.write_bytes(b"protected")
    with pytest.raises(FileExistsError): run_job(job_file(tmp_path, "prepare", source))
    assert protected.read_bytes() == b"protected"


@pytest.mark.parametrize("mutation", [
    {"unknown": 1}, {"schema_version": 2}, {"operation": "arbitrary_python"},
    {"input": {"path": "anything", "start_frame": -1}}, {"params": {"typo_gain": 1}}
])
def test_invalid_contracts_fail_before_output(audio_file, tmp_path, mutation):
    source, _, _ = audio_file
    job = job_file(tmp_path, "prepare", source)
    request = read_json(job)
    request.update(mutation)
    job.write_text(json.dumps(request))
    with pytest.raises(Exception): run_job(job)
    assert not (tmp_path / "result").exists()


@pytest.mark.parametrize("text", ['{"x": NaN}', '{"x":1e999}', '{"x":1,"x":2}'])
def test_ambiguous_json_is_rejected(tmp_path, text):
    path = tmp_path / "bad.json"
    path.write_text(text)
    with pytest.raises(ValueError): read_json(path)


def test_nonfinite_source_fails_without_completion_marker(tmp_path):
    path = tmp_path / "nonfinite.wav"
    sf.write(path, np.array([[0, np.nan]], np.float32), 44100, subtype="FLOAT")
    with pytest.raises(ValueError, match="NaN"): run_job(job_file(tmp_path, "prepare", path))
    assert not (tmp_path / "result/result.json").exists()
    assert (tmp_path / "result/failure.json").exists()


def test_cache_verifies_audio_and_invalidates_changed_source(audio_file, tmp_path):
    source, original, rate = audio_file
    first = run_job(job_file(tmp_path, "prepare", source, output="one"))["result"]
    second = run_job(job_file(tmp_path, "prepare", source, output="two"))["result"]
    assert not first["cache_hit"] and second["cache_hit"]
    assert first["job_sha256"] == second["job_sha256"]
    cached = tmp_path / "cache/objects" / first["job_sha256"] / "audio.wav"
    cached.write_bytes(b"corrupt cached file")
    third = run_job(job_file(tmp_path, "prepare", source, output="three"))["result"]
    assert not third["cache_hit"]
    assert np.array_equal(sf.read(tmp_path / "three/audio.wav", dtype="float32")[0], original)
    sf.write(source, original * .5, rate, subtype="FLOAT")
    changed = run_job(job_file(tmp_path, "prepare", source, output="four"))["result"]
    assert changed["job_sha256"] != first["job_sha256"]


def test_effect_stream_preserves_state_and_explicit_tail(audio_file, tmp_path):
    import pedalboard as p
    source, samples, rate = audio_file
    tail = 1000
    params = {"tail_frames": tail, "chain": [
        {"type": "HighpassFilter", "params": {"cutoff_frequency_hz": 100}},
        {"type": "Delay", "params": {"delay_seconds": .03, "feedback": .2, "mix": .3}}
    ]}
    run_job(job_file(tmp_path, "effects", source, params))
    actual, _ = sf.read(tmp_path / "result/audio.wav", dtype="float32", always_2d=True)
    count = len(samples) + tail
    padded = np.zeros((2, ((count + 8191) // 8192) * 8192), np.float32)
    padded[:, :len(samples)] = samples.T
    expected = p.Pedalboard([p.HighpassFilter(cutoff_frequency_hz=100), p.Delay(delay_seconds=.03, feedback=.2, mix=.3)])(padded, rate)[:, :count].T
    assert actual.shape == (count, 2)
    assert np.array_equal(actual, expected)


def test_one_frame_stereo_effect_keeps_channel_order(tmp_path):
    source = tmp_path / "one.wav"
    original = np.array([[.2, -.4]], np.float32)
    sf.write(source, original, 44100, subtype="FLOAT")
    run_job(job_file(tmp_path, "effects", source, {"chain": [{"type": "Gain", "params": {"gain_db": 0}}]}))
    assert np.array_equal(sf.read(tmp_path / "result/audio.wav", dtype="float32", always_2d=True)[0], original)


def test_resample_changes_rate_and_preserves_duration(audio_file, tmp_path):
    source, original, rate = audio_file
    run_job(job_file(tmp_path, "prepare", source, {"sample_rate": 48000}))
    audio, new_rate = sf.read(tmp_path / "result/audio.wav", dtype="float32", always_2d=True)
    assert new_rate == 48000 and audio.shape[1] == 2
    assert abs(len(audio) - round(len(original) * 48000 / rate)) <= 1
    assert np.isfinite(audio).all()


def test_hpss_preserves_length_and_reconstructs_input(audio_file, tmp_path):
    source, original, _ = audio_file
    run_job(job_file(tmp_path, "decompose", source, {"margin": 2}))
    layers = [sf.read(tmp_path / "result" / name, dtype="float32")[0]
              for name in ["harmonic.wav", "percussive.wav", "residual.wav"]]
    assert all(x.shape == original.shape for x in layers)
    np.testing.assert_allclose(sum(layers), original, atol=3e-8, rtol=1e-6)


def test_real_schema_validation_and_remote_reference_rejection(tmp_path):
    source = tmp_path / "document.json"
    schema = tmp_path / "schema.json"
    source.write_text('{"frames": 10}')
    schema.write_text(json.dumps({"type": "object", "properties": {"frames": {"type": "integer", "minimum": 1}}, "required": ["frames"]}))
    result = run_job(job_file(tmp_path, "validate", source, {"schema": str(schema)}))
    assert result["result"]["details"]["valid"]
    source.write_text('{"frames": -1}')
    with pytest.raises(Exception): run_job(job_file(tmp_path, "validate", source, {"schema": str(schema)}, output="invalid"))
    schema.write_text('{"$ref": "https://example.invalid/not-fetched.json"}')
    with pytest.raises(Exception): run_job(job_file(tmp_path, "validate", source, {"schema": str(schema)}, output="remote"))
    assert not (tmp_path / "remote/result.json").exists()


def test_decode_and_export_work_offline(audio_file, tmp_path):
    source, samples, rate = audio_file
    run_job(job_file(tmp_path, "decode", source, output="decoded"))
    decoded, actual_rate = sf.read(tmp_path / "decoded/audio.wav", dtype="float32")
    assert actual_rate == rate and np.array_equal(decoded, samples)
    run_job(job_file(tmp_path, "export", source, {"bitrate_kbps": 192}, output="encoded"))
    assert (tmp_path / "encoded/audio.mp3").stat().st_size > 1000


def test_chip_matches_recorded_production_note_fixtures():
    from mam_tools.chip import square_note
    fixtures = read_json(Path(__file__).with_name("chip_golden.json"))
    for case in fixtures["cases"]:
        audio = square_note(**case["parameters"])
        assert hashlib.sha256(audio.tobytes()).hexdigest() == case["sample_sha256"]


def test_chip_render_and_overlap_rejection(tmp_path):
    params = {"frames": 22050, "tone": "bass", "events": [
        {"start_frame": 0, "frames": 11025, "midi": 43},
        {"start_frame": 11025, "frames": 11025, "midi": 48}
    ]}
    result = run_job(job_file(tmp_path, "chip", params=params))
    assert result["result"]["artifacts"]["audio.wav"]["frames"] == 22050
    params["events"][1]["start_frame"] = 100
    with pytest.raises(ValueError, match="overlap"):
        run_job(job_file(tmp_path, "chip", params=params, output="overlap"))
