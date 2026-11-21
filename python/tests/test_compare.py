import json
import subprocess

import numpy as np
import pytest
import soundfile as sf

from mam_tools.compare import ComparisonMismatch
from mam_tools.engine import run_job


def request(tmp_path, actual, reference, name='comparison', **params):
    job = {'schema_version': 1, 'operation': 'compare', 'output_dir': name,
           'input': {'path': str(actual)},
           'params': {'reference': {'path': str(reference)}, **params}}
    path = tmp_path / f'{name}.json'
    path.write_text(json.dumps(job))
    return path


def test_exact_offset_windows_across_read_blocks(tmp_path):
    samples = np.zeros((131081, 2), np.float32)
    samples[[0, 65535, 65536, 131072, 131080]] = [.25, -.5]
    left, right = tmp_path / 'left.wav', tmp_path / 'right.wav'
    sf.write(left, np.pad(samples, ((3, 4), (0, 0))), 44100, subtype='FLOAT')
    sf.write(right, np.pad(samples, ((7, 1), (0, 0))), 44100, subtype='PCM_24')
    path = request(tmp_path, left.name, right.name)
    job = json.loads(path.read_text())
    job['input'].update(start_frame=3, frames=len(samples))
    job['params']['reference'].update(start_frame=7, frames=len(samples))
    path.write_text(json.dumps(job))
    report = run_job(path)['result']['details']
    assert report['sample_equal'] and report['within_tolerance']
    assert report['frames'] == len(samples)
    assert report['max_abs_error'] == report['rms_error'] == 0
    assert report['max_error_dbfs'] is None
    assert report['first_difference'] is None


def test_double_precision_difference_is_not_rounded_away(tmp_path):
    left, right = tmp_path / 'left.wav', tmp_path / 'right.wav'
    sf.write(left, np.array([[.5, .25]], np.float64), 48000, subtype='DOUBLE')
    sf.write(right, np.array([[.5, .25 + 2**-40]], np.float64), 48000, subtype='DOUBLE')
    report = run_job(request(tmp_path, left, right, max_abs_error=2**-40))['result']['details']
    assert not report['sample_equal'] and report['within_tolerance']
    assert report['max_abs_error'] == 2**-40
    assert report['unequal_samples'] == 1
    assert report['first_difference'] == {'frame': 0, 'channel': 1}


def test_difference_fails_and_preserves_diagnostic_without_success(tmp_path):
    left, right = tmp_path / 'left.wav', tmp_path / 'right.wav'
    samples = np.zeros((65538, 2), np.float32)
    sf.write(right, samples, 44100, subtype='FLOAT')
    samples[65536, 1] = .25
    samples[65537, 0] = -.5
    sf.write(left, samples, 44100, subtype='FLOAT')
    with pytest.raises(ComparisonMismatch):
        run_job(request(tmp_path, left, right, max_abs_error=.125))
    assert not (tmp_path / 'comparison/result.json').exists()
    failure = json.loads((tmp_path / 'comparison/failure.json').read_text())['comparison']
    assert failure['max_abs_error'] == .5
    assert failure['rms_error'] == pytest.approx(np.sqrt(.3125 / samples.size))
    assert failure['samples_above_tolerance'] == 2
    assert failure['first_above_tolerance'] == {'frame': 65536, 'channel': 1}
    assert failure['maximum_error_location'] == {'frame': 65537, 'channel': 0}


def test_one_frame_shift_is_not_auto_aligned(tmp_path):
    left, right = tmp_path / 'left.wav', tmp_path / 'right.wav'
    samples = np.zeros((100, 1), np.float32)
    samples[30] = .5
    sf.write(left, samples, 44100, subtype='FLOAT')
    sf.write(right, np.roll(samples, 1, axis=0), 44100, subtype='FLOAT')
    report = run_job(request(tmp_path, left, right, fail_on_difference=False))['result']['details']
    assert not report['within_tolerance'] and not report['alignment_compensated']
    assert report['unequal_samples'] == 2


@pytest.mark.parametrize('rate,channels,frames', [(48000,2,100), (44100,1,100), (44100,2,101)])
def test_format_or_length_mismatch_is_rejected(tmp_path, rate, channels, frames):
    left, right = tmp_path / 'left.wav', tmp_path / 'right.wav'
    sf.write(left, np.zeros((100,2), np.float32), 44100, subtype='FLOAT')
    sf.write(right, np.zeros((frames,channels), np.float32), rate, subtype='FLOAT')
    with pytest.raises(ValueError, match='equal sample rate'):
        run_job(request(tmp_path, left, right))


@pytest.mark.parametrize('bad_side', ['left', 'right'])
def test_nonfinite_on_either_side_fails(tmp_path, bad_side):
    paths = {name: tmp_path / f'{name}.wav' for name in ['left', 'right']}
    for name, path in paths.items():
        sf.write(path, np.array([np.nan if name == bad_side else 0], np.float32), 44100, subtype='FLOAT')
    with pytest.raises(ValueError, match='NaN'):
        run_job(request(tmp_path, paths['left'], paths['right']))


def test_cache_fingerprints_reference_and_invalidates_change(audio_file, tmp_path):
    source, samples, rate = audio_file
    reference = tmp_path / 'reference.wav'
    sf.write(reference, samples, rate, subtype='FLOAT')
    first = run_job(request(tmp_path, source, reference, 'first', fail_on_difference=False))['result']
    second = run_job(request(tmp_path, source, reference, 'second', fail_on_difference=False))['result']
    assert second['cache_hit']
    assert len(second['recipe']['source_sha256']) == 2
    sf.write(reference, samples * .5, rate, subtype='FLOAT')
    third = run_job(request(tmp_path, source, reference, 'third', fail_on_difference=False))['result']
    assert not third['cache_hit'] and not third['details']['sample_equal']
    assert first['job_sha256'] != third['job_sha256']


def test_negative_tolerance_rejected_before_output(audio_file, tmp_path):
    source, _, _ = audio_file
    with pytest.raises(Exception):
        run_job(request(tmp_path, source, source, max_abs_error=-.01))
    assert not (tmp_path / 'comparison').exists()


def test_comparison_failure_propagates_to_native_cli(audio_file, tmp_path, binary):
    source, samples, rate = audio_file
    other = tmp_path / 'other.wav'
    sf.write(other, samples * .5, rate, subtype='FLOAT')
    job = request(tmp_path, source, other)
    process = subprocess.run([binary, 'tools', 'run', str(job)], capture_output=True, text=True, timeout=30)
    assert process.returncode != 0
    assert json.loads(process.stdout)['error_type'] == 'ComparisonMismatch'
