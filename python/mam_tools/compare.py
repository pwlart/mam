"""Streaming, aligned PCM comparisons without gain or timing compensation."""
import math

from .audio import BLOCK, db, source_window


class ComparisonMismatch(ValueError):
    def __init__(self, details):
        self.comparison = details
        super().__init__(f"Audio difference {details['max_abs_error']} exceeds "
                         f"tolerance {details['tolerance']}")


def compare_audio(source, params):
    import numpy as np

    reference = params['reference']
    tolerance = params.get('max_abs_error', 0.0)
    maximum = square_sum = 0.0
    unequal = outside = position = 0
    first_difference = first_outside = worst = None
    with source_window(source) as (actual, frames), source_window(reference) as (expected, reference_frames):
        if (actual.samplerate, actual.channels, frames) != (expected.samplerate, expected.channels, reference_frames):
            raise ValueError('Comparison windows must have equal sample rate, channels and frame count')
        rate, channels = actual.samplerate, actual.channels
        while position < frames:
            count = min(BLOCK, frames - position)
            # Float64 also preserves DOUBLE WAV differences smaller than a
            # Float32 ULP. No resampling, alignment search or gain matching.
            left = actual.read(count, dtype='float64', always_2d=True)
            right = expected.read(count, dtype='float64', always_2d=True)
            if len(left) != count or len(right) != count:
                raise ValueError('Unexpected end of comparison input')
            if not np.isfinite(left).all() or not np.isfinite(right).all():
                raise ValueError('Comparison audio contains NaN or infinity')
            with np.errstate(over='ignore', invalid='ignore'):
                delta = left - right
                absolute = np.abs(delta)
                energy = float(np.sum(delta * delta))
            if not np.isfinite(absolute).all() or not math.isfinite(energy + square_sum):
                raise ValueError('Comparison arithmetic exceeds Float64 range')
            different = absolute > 0
            exceeds = absolute > tolerance
            unequal += int(np.count_nonzero(different))
            outside += int(np.count_nonzero(exceeds))
            def location(flat):
                frame, channel = divmod(int(flat), channels)
                return {'frame': position + frame, 'channel': channel}
            if first_difference is None and different.any():
                first_difference = location(np.flatnonzero(different)[0])
            if first_outside is None and exceeds.any():
                first_outside = location(np.flatnonzero(exceeds)[0])
            block_max = float(absolute.max())
            if block_max > maximum:
                maximum, worst = block_max, location(np.argmax(absolute))
            square_sum += energy
            position += count
    rms = math.sqrt(square_sum / (frames * channels))
    details = {
        'sample_rate': rate, 'channels': channels, 'frames': frames,
        'input_start_frame': source.get('start_frame', 0),
        'reference_start_frame': reference.get('start_frame', 0),
        'comparison_format': 'Float64 decoded PCM', 'alignment_compensated': False,
        'gain_compensated': False, 'sample_equal': unequal == 0,
        'within_tolerance': outside == 0, 'tolerance': tolerance,
        'max_abs_error': maximum, 'max_error_dbfs': db(maximum),
        'rms_error': rms, 'rms_error_dbfs': db(rms),
        'unequal_samples': unequal, 'samples_above_tolerance': outside,
        'first_difference': first_difference, 'first_above_tolerance': first_outside,
        'maximum_error_location': worst,
        'location_units': 'zero-based frame within selected window and channel',
    }
    if outside and params.get('fail_on_difference', True):
        raise ComparisonMismatch(details)
    return details
