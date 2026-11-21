from pathlib import Path
import os
import sys

import numpy as np
import pytest
import soundfile as sf

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))


@pytest.fixture(autouse=True)
def private_cache(tmp_path, monkeypatch):
    monkeypatch.setenv("MAM_CACHE_DIR", str(tmp_path / "cache"))
    monkeypatch.setenv("NUMBA_CACHE_DIR", str(tmp_path / "numba"))


@pytest.fixture
def audio_file(tmp_path):
    rate = 44100
    index = np.arange(24001, dtype=np.float32)
    samples = np.stack([np.sin(index * .031) * .2, np.cos(index * .047) * .1], axis=1).astype(np.float32)
    path = tmp_path / "source with spaces.wav"
    sf.write(path, samples, rate, subtype="FLOAT")
    return path, samples, rate


@pytest.fixture
def binary():
    path = os.environ.get("MAM_TEST_BINARY")
    if not path:
        pytest.skip("Native integration requires MAM_TEST_BINARY; use the CMake python preset")
    return str(Path(path).resolve())
