import json
import os
from pathlib import Path
import subprocess
import sys
import time

import numpy as np
import pytest
import soundfile as sf


def invoke(binary, *arguments, timeout=30):
    return subprocess.run([binary, *map(str, arguments)], capture_output=True, text=True,
                          env={**os.environ, "MAM_PYTHON": sys.executable}, timeout=timeout)


def write_job(path, job):
    path.write_text(json.dumps(job))
    return path


def stem_job(source, output="native"):
    return {"schema_version": 1, "sample_rate": 44100, "channels": 2,
            "frames": 20000, "output_dir": output,
            "stems": [{"path": str(source), "source_start_frame": 8191,
                       "destination_start_frame": 123, "frames": 10000}]}


def test_cpp_launches_worker_without_a_shell(binary, audio_file, tmp_path):
    source, samples, _ = audio_file
    path = tmp_path / "job $(touch SHOULD_NOT_EXIST) 'quote'.json"
    write_job(path, {"schema_version": 1, "operation": "prepare", "output_dir": "python result",
                     "input": {"path": str(source)}})
    process = invoke(binary, "tools", "run", path)
    assert process.returncode == 0, process.stderr
    report = json.loads(process.stdout)
    assert report["status"] == "ok"
    assert not (tmp_path / "SHOULD_NOT_EXIST").exists()
    assert np.array_equal(sf.read(tmp_path / "python result/audio.wav", dtype="float32")[0], samples)


def test_doctor_and_missing_interpreter(binary):
    process = invoke(binary, "tools", "doctor")
    assert process.returncode == 0, process.stderr
    assert json.loads(process.stdout)["capabilities"]["effects"]
    missing = invoke(binary, "tools", "--python", "/nonexistent/mam-python", "doctor")
    assert missing.returncode == 127


def test_worker_failure_and_timeout_are_propagated(binary, tmp_path):
    worker = tmp_path / "worker.py"
    worker.write_text("raise SystemExit(17)\n")
    process = invoke(binary, "tools", "--worker-dir", tmp_path, "doctor")
    assert process.returncode == 17
    worker.write_text("import signal,time\nsignal.signal(signal.SIGTERM, signal.SIG_IGN)\ntime.sleep(30)\n")
    started = time.monotonic()
    process = invoke(binary, "tools", "--worker-dir", tmp_path, "--timeout", ".25", "doctor", timeout=8)
    assert process.returncode == 124
    assert time.monotonic() - started < 5
    assert "timed out" in process.stderr


def test_native_placement_is_exact_and_independent_of_block_size(binary, audio_file, tmp_path):
    source, samples, rate = audio_file
    expected = np.zeros((20000, 2), np.float32)
    expected[123:10123] = samples[8191:18191]
    hashes = []
    for block in [257, 4096, 65536]:
        job = stem_job(source, f"native-{block}")
        job["block_frames"] = block
        process = invoke(binary, "stems", "render", write_job(tmp_path / f"job-{block}.json", job))
        assert process.returncode == 0, process.stderr
        report = json.loads(process.stdout)
        actual, actual_rate = sf.read(tmp_path / f"native-{block}/audio.wav", dtype="float32")
        assert actual_rate == rate and np.array_equal(actual, expected)
        hashes.append(report["sample_sha1"])
    assert len(set(hashes)) == 1


def test_native_overlap_fades_and_gain_match_reference(binary, tmp_path):
    source = tmp_path / "constant.wav"
    sf.write(source, np.ones((2000, 2), np.float32) * .5, 44100, subtype="FLOAT")
    job = {"schema_version": 1, "sample_rate": 44100, "channels": 2, "frames": 3000,
           "output_dir": "mix", "block_frames": 257,
           "stems": [
               {"path": str(source), "destination_start_frame": 0, "frames": 2000, "fade_out_frames": 1000},
               {"path": str(source), "destination_start_frame": 1000, "frames": 2000, "fade_in_frames": 1000,
                "gain_db": -6.020599913279624}
           ]}
    process = invoke(binary, "stems", "render", write_job(tmp_path / "mix.json", job))
    assert process.returncode == 0, process.stderr
    actual, _ = sf.read(tmp_path / "mix/audio.wav", dtype="float32")
    ramp = np.sin(np.linspace(0, np.pi / 2, 1000)) ** 2
    expected = np.zeros(3000)
    expected[:2000] += .5 * np.r_[np.ones(1000), ramp[::-1]]
    expected[1000:] += .25 * np.r_[ramp, np.ones(1000)]
    np.testing.assert_allclose(actual[:, 0], expected, atol=5e-8, rtol=1e-6)
    assert np.array_equal(actual[:, 0], actual[:, 1])
    assert actual[0, 0] == .5 and actual[-1, 0] == .25


@pytest.mark.parametrize("field,value", [
    ("sample_rate", 48000), ("channels", 1), ("frames", 100),
    ("frames", -1), ("block_frames", 0), ("block_frames", 100000000),
    ("unexpected", True)
])
def test_invalid_native_jobs_do_not_create_outputs(binary, audio_file, tmp_path, field, value):
    source, _, _ = audio_file
    job = stem_job(source)
    job[field] = value
    process = invoke(binary, "stems", "render", write_job(tmp_path / "bad.json", job))
    assert process.returncode != 0
    assert not (tmp_path / "native").exists()


def test_native_rejects_existing_directory(binary, audio_file, tmp_path):
    source, _, _ = audio_file
    output = tmp_path / "native"
    output.mkdir()
    (output / "audio.wav").write_bytes(b"existing master")
    process = invoke(binary, "stems", "render", write_job(tmp_path / "bad.json", stem_job(source)))
    assert process.returncode != 0
    assert (output / "audio.wav").read_bytes() == b"existing master"


def test_native_rejects_duplicate_keys(binary, tmp_path):
    job = tmp_path / "duplicate.json"
    job.write_text('{"schema_version": 1, "schema_version": 2}')
    process = invoke(binary, "stems", "render", job)
    assert process.returncode != 0
    assert "Duplicate JSON key" in process.stderr


def test_native_rejects_nonfinite_input_without_success_marker(binary, tmp_path):
    source = tmp_path / "nan.wav"
    data = np.zeros((20000, 2), np.float32)
    data[10000, 1] = np.nan
    sf.write(source, data, 44100, subtype="FLOAT")
    process = invoke(binary, "stems", "render", write_job(tmp_path / "bad.json", stem_job(source)))
    assert process.returncode != 0
    assert not (tmp_path / "native/result.json").exists()


def test_python_processed_stem_can_be_rendered_natively(binary, audio_file, tmp_path):
    source, samples, _ = audio_file
    request = {"schema_version": 1, "operation": "effects", "output_dir": "processed",
               "input": {"path": str(source)}, "params": {"chain": [{"type": "Gain", "params": {"gain_db": 0}}]}}
    process = invoke(binary, "tools", "run", write_job(tmp_path / "effects.json", request))
    assert process.returncode == 0, process.stderr
    job = stem_job(tmp_path / "processed/audio.wav")
    process = invoke(binary, "stems", "render", write_job(tmp_path / "native.json", job))
    assert process.returncode == 0, process.stderr
    actual, _ = sf.read(tmp_path / "native/audio.wav", dtype="float32")
    assert np.array_equal(actual[123:10123], samples[8191:18191])


@pytest.mark.parametrize("subtype", ["PCM_16", "PCM_24"])
def test_native_decodes_integer_pcm_like_python(binary, tmp_path, subtype):
    samples = np.repeat(np.linspace(-.8, .8, 24001, dtype=np.float32)[:, None], 2, axis=1)
    source = tmp_path / "integer.wav"
    sf.write(source, samples, 44100, subtype=subtype)
    decoded, _ = sf.read(source, dtype="float32")
    process = invoke(binary, "stems", "render", write_job(tmp_path / "integer.json", stem_job(source)))
    assert process.returncode == 0, process.stderr
    actual, _ = sf.read(tmp_path / "native/audio.wav", dtype="float32")
    assert np.array_equal(actual[123:10123], decoded[8191:18191])
