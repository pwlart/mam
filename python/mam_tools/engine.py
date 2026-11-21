"""File-based jobs with exclusive outputs, provenance and verified result reuse."""
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile

from . import __version__
from .contracts import read_json, resolve_path, validate_document, validate_job

PACKAGES = ["numpy", "scipy", "soundfile", "soxr", "pedalboard", "librosa", "pyloudnorm",
            "praat-parselmouth", "matplotlib", "jsonschema", "referencing", "pytest", "numba", "llvmlite"]


def sha(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def versions():
    result = {}
    for name in PACKAGES:
        try: result[name] = importlib.metadata.version(name)
        except importlib.metadata.PackageNotFoundError: result[name] = None
    return result


def runtime():
    ffmpeg = shutil.which("ffmpeg")
    ffmpeg_version = None
    if ffmpeg:
        process = subprocess.run([ffmpeg, "-version"], capture_output=True, text=True, timeout=10, check=True)
        ffmpeg_version = process.stdout.splitlines()[0]
    root = Path(__file__).parent
    implementation = {p.name: sha(p) for p in sorted(root.iterdir()) if p.suffix in {".py", ".json"}}
    return {"worker_version": __version__, "python": platform.python_version(),
            "architecture": platform.machine(), "system": platform.platform(),
            "packages": versions(), "ffmpeg": ffmpeg_version,
            "implementation_sha256": implementation}


def write_json(path, value):
    with Path(path).open("x", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2, allow_nan=False)
        stream.write("\n")


def artifacts(directory):
    result = {}
    for path in sorted(directory.iterdir()):
        if path.name == "result.json" or not path.is_file(): continue
        entry = {"sha256": sha(path), "bytes": path.stat().st_size}
        if path.suffix == ".wav":
            import soundfile as sf
            info = sf.info(path)
            entry.update(frames=info.frames, sample_rate=info.samplerate,
                         channels=info.channels, subtype=info.subtype)
        result[path.name] = entry
    return result


def valid_cache(directory, key):
    try:
        report = read_json(directory / "result.json")
        if report["status"] != "ok" or report["job_sha256"] != key: return None
        for name, info in report["artifacts"].items():
            if Path(name).name != name: return None
            path = directory / name
            if path.is_symlink() or not path.is_file() or sha(path) != info["sha256"]: return None
        return report
    except (OSError, ValueError, KeyError, TypeError):
        return None


def run_job(job_path):
    from . import audio
    path = Path(job_path).resolve()
    job = read_json(path)
    validate_job(job)
    output = resolve_path(job["output_dir"], path.parent)
    if output.exists(): raise FileExistsError(f"Output directory already exists: {output}")
    if not output.parent.is_dir(): raise ValueError("Output parent must already exist")
    operation, params = job["operation"], dict(job.get("params", {}))
    source = dict(job.get("input", {}))
    inputs = {}
    if source:
        source["path"] = str(resolve_path(source["path"], path.parent))
        inputs[source["path"]] = sha(source["path"])
    if operation == "validate":
        params["schema"] = str(resolve_path(params["schema"], path.parent))
        inputs[params["schema"]] = sha(params["schema"])
    if operation == "compare":
        params["reference"] = dict(params["reference"])
        params["reference"]["path"] = str(resolve_path(params["reference"]["path"], path.parent))
        inputs[params["reference"]["path"]] = sha(params["reference"]["path"])
    environment = runtime()
    recipe = {"schema_version": 1, "operation": operation, "params": params,
              "input": source, "source_sha256": inputs, "runtime": environment}
    key = hashlib.sha256(json.dumps(recipe, sort_keys=True, allow_nan=False).encode()).hexdigest()
    cache_root = Path(os.environ.get("MAM_CACHE_DIR", Path(__file__).resolve().parents[2] / "build/python-cache")).resolve()
    cache_entry = cache_root / "objects" / key
    cached = valid_cache(cache_entry, key) if job.get("cache", True) else None
    output.mkdir()  # exclusive reservation; result.json is the completion marker
    try:
        with tempfile.TemporaryDirectory(prefix=".work-", dir=output) as temporary:
            work = Path(temporary)
            if cached:
                for name in cached["artifacts"]: shutil.copyfile(cache_entry / name, work / name)
                if artifacts(work) != cached["artifacts"]:
                    raise ValueError("Cached result changed during copy")
                report = dict(cached)
                report["cache_hit"] = True
            else:
                if operation == "inspect":
                    details = audio.inspect_audio(source, params.get("loudness", True))
                    write_json(work / "analysis.json", details)
                elif operation == "compare":
                    from .compare import compare_audio
                    details = compare_audio(source, params)
                    write_json(work / "comparison.json", details)
                elif operation == "prepare":
                    details = audio.prepare(source, params, work / "audio.wav")
                elif operation == "effects":
                    details = audio.effects(source, params, work / "audio.wav")
                elif operation == "decompose":
                    details = audio.decompose(source, params, work)
                elif operation == "chip":
                    from .chip import render
                    details = render(params, work / "audio.wav")
                elif operation == "validate":
                    validate_document(read_json(source["path"]), read_json(params["schema"]))
                    details = {"valid": True, "draft": "2020-12", "remote_references": "disabled"}
                    write_json(work / "validation.json", details)
                elif operation == "decode":
                    args = ["-v", "error", "-n", "-i", source["path"], "-map", "0:a:0", "-vn"]
                    if "sample_rate" in params: args += ["-ar", str(params["sample_rate"])]
                    audio.ffmpeg(args + ["-c:a", "pcm_f32le", str(work / "audio.wav")])
                    details = audio.inspect_audio({"path": str(work / "audio.wav")}, False)
                elif operation == "export":
                    audio.inspect_audio(source, False)  # reject invalid samples before encoding
                    audio.ffmpeg(["-v", "error", "-n", "-i", source["path"], "-map", "0:a:0", "-vn",
                                  "-c:a", "libmp3lame", "-b:a", f"{params.get('bitrate_kbps', 320)}k", str(work / "audio.mp3")])
                    details = {"codec": "libmp3lame", "lossy": True, "bitrate_kbps": params.get("bitrate_kbps", 320)}
                report = {"schema_version": 1, "status": "ok", "job_sha256": key,
                          "cache_hit": False, "recipe": recipe, "details": details,
                          "artifacts": artifacts(work)}
            # Hash again before publishing: never attribute a render to a source
            # that changed while the worker was reading it.
            if any(sha(name) != digest for name, digest in inputs.items()):
                raise ValueError("Input changed during the job")
            for name in report["artifacts"]:
                os.link(work / name, output / name)  # atomic create; never replaces a file
            write_json(output / "result.json", report)
        if job.get("cache", True) and not cached:
            try:
                cache_entry.parent.mkdir(parents=True, exist_ok=True)
                with tempfile.TemporaryDirectory(prefix=".cache-", dir=cache_entry.parent) as temporary:
                    staging = Path(temporary) / "result"
                    staging.mkdir()
                    for name in [*report["artifacts"], "result.json"]:
                        shutil.copyfile(output / name, staging / name)
                    try: staging.rename(cache_entry)
                    except OSError:
                        # Another worker may already have published this immutable key.
                        if not cache_entry.exists(): raise
            except OSError as error:
                print(f"MAM tools: result completed; cache unavailable: {error}", file=sys.stderr)
        return {"status": "ok", "output_dir": str(output), "result": report}
    except Exception as error:
        if not (output / "result.json").exists():
            failure = {"status": "failed", "error": str(error)}
            from .compare import ComparisonMismatch
            if isinstance(error, ComparisonMismatch):
                failure["comparison"] = error.comparison
            write_json(output / "failure.json", failure)
        raise
