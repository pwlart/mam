import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import sys


def doctor():
    from .engine import versions
    available = versions()
    requirements = {
        "inspect": ["numpy", "soundfile", "jsonschema"],
        "compare": ["numpy", "soundfile", "jsonschema"],
        "prepare": ["numpy", "soundfile", "soxr", "jsonschema"],
        "effects": ["numpy", "soundfile", "pedalboard", "jsonschema"],
        "decompose": ["numpy", "soundfile", "librosa", "jsonschema"],
        "chip": ["numpy", "scipy", "soundfile", "jsonschema"],
        "validate": ["jsonschema"],
    }
    capabilities = {name: all(available[p] is not None for p in packages)
                    for name, packages in requirements.items()}
    capabilities["decode"] = capabilities["export"] = bool(shutil.which("ffmpeg")) and capabilities["inspect"]
    plugins = []
    for root in [Path("/Library/Audio/Plug-Ins"), Path.home() / "Library/Audio/Plug-Ins"]:
        for kind, extension in [("VST3", "*.vst3"), ("Components", "*.component")]:
            plugins.extend({"path": str(p), "tested": False} for p in sorted((root / kind).glob(extension)))
    return {"status": "ok", "python_executable": sys.executable, "python_version": platform.python_version(),
            "architecture": platform.machine(), "packages": available, "capabilities": capabilities,
            "ffmpeg": shutil.which("ffmpeg"), "ffprobe": shutil.which("ffprobe"),
            "espeak_ng": shutil.which("espeak-ng"), "plugins_present_not_hosted": plugins,
            "note": "Capabilities report package presence, not a successful plugin load or DSP audition."}


def main(argv=None):
    parser = argparse.ArgumentParser(description="MAM optional offline Python tools")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("doctor", help="report interpreter, packages and capabilities without rendering")
    run = commands.add_parser("run", help="execute a versioned JSON job into a new output directory")
    run.add_argument("job")
    args = parser.parse_args(argv)
    # Keep lazy-import caches away from the user's home and protected media.
    cache = Path(os.environ.get("MAM_CACHE_DIR", Path(__file__).resolve().parents[2] / "build/python-cache"))
    os.environ.setdefault("NUMBA_CACHE_DIR", str(cache / "numba"))
    os.environ.setdefault("MPLCONFIGDIR", str(cache / "matplotlib"))
    try:
        if args.command == "doctor": result = doctor()
        else:
            from .engine import run_job
            result = run_job(args.job)
        print(json.dumps(result, allow_nan=False))
        return 0
    except Exception as error:
        print(json.dumps({"status": "error", "error_type": type(error).__name__, "message": str(error)}, allow_nan=False))
        print(f"MAM tools: {error}", file=sys.stderr)
        return 1
