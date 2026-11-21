#!/usr/bin/env python3
"""Check publishable files and relative Markdown links without external services."""
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
MEDIA = {".wav", ".aiff", ".aif", ".caf", ".mp3", ".m4a", ".flac", ".ogg", ".opus",
         ".mp4", ".mov", ".zip", ".gz", ".tar", ".bundle", ".o", ".a", ".dylib", ".pfx", ".p12"}
BUILD_PART = re.compile(r"^(build(?:[.-].*)?|\.venv|__pycache__|\.pytest_cache|dist)$")
HOME_PATH = re.compile(r"/(?:Users|home)/[A-Za-z0-9_.-]+/")
LINK = re.compile(r"!?\[[^\]]*\]\(([^)\s]+)(?:\s+[\"'][^)]*)?\)")
CREDENTIAL_NAMES = {".netrc", ".npmrc", ".pypirc", "credentials.json", "id_rsa", "id_ed25519", "id_ecdsa"}


def source_files():
    result = subprocess.run(["git", "-C", str(ROOT), "ls-files", "--cached",
                             "--others", "--exclude-standard", "-z"],
                            check=True, capture_output=True)
    return sorted({ROOT / name.decode() for name in result.stdout.split(b"\0") if name})


def check():
    errors = []
    paths = source_files()
    for path in paths:
        relative = path.relative_to(ROOT)
        if not path.exists():
            errors.append(f"{relative}: indexed file missing")
            continue
        if path.is_symlink() or not path.is_file():
            errors.append(f"{relative}: source must be a regular file")
            continue
        if any(BUILD_PART.match(part) for part in relative.parts):
            errors.append(f"{relative}: generated build/environment path")
        if path.suffix.lower() in MEDIA or path.name in CREDENTIAL_NAMES or (
                path.name.startswith(".env") and path.name != ".env.example"):
            errors.append(f"{relative}: artifact or credential filename")
        data = path.read_bytes()
        if len(data) > 2 * 1024 * 1024 or b"\0" in data:
            errors.append(f"{relative}: oversized or binary source")
            continue
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError:
            errors.append(f"{relative}: non-UTF-8 source")
            continue
        if not relative.parts[0] == "vendor" and HOME_PATH.search(text):
            errors.append(f"{relative}: personal home-directory path")
        if path.suffix.lower() == ".md":
            for target in LINK.findall(text):
                if "://" in target or target.startswith(("mailto:", "#")):
                    continue
                target = target.split("#", 1)[0].split("?", 1)[0]
                if target and not (path.parent / target).exists():
                    errors.append(f"{relative}: missing relative link {target}")
    for error in errors:
        print(error, file=sys.stderr)
    print(f"Repository hygiene: {len(paths)} files, {len(errors)} errors.")
    return bool(errors)


if __name__ == "__main__":
    raise SystemExit(check())
