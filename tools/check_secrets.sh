#!/usr/bin/env bash
# Scan publishable files and current repository history locally. No uploads.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
scanner="${GITLEAKS:-gitleaks}"
if ! command -v "$scanner" >/dev/null 2>&1; then
  echo "Install Gitleaks explicitly (for example: brew install gitleaks), or set GITLEAKS." >&2
  exit 127
fi
work="$(mktemp -d "${TMPDIR:-/tmp}/mam-secret-check.XXXXXX")"
trap 'rm -rf "$work"' EXIT
python3 - "$root" "$work/source" <<'PY'
from pathlib import Path
import shutil, subprocess, sys
root, dest = map(Path, sys.argv[1:])
dest.mkdir()
files = subprocess.check_output(["git", "-C", str(root), "ls-files", "--cached",
                                "--others", "--exclude-standard", "-z"])
for name in sorted(set(files.split(b"\0")) - {b""}):
    relative = Path(name.decode())
    source = root / relative
    if source.is_symlink() or not source.is_file():
        raise SystemExit("Publishable entries must be regular files: " + str(relative))
    output = dest / relative
    output.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, output)
PY
touch "$work/empty.ignore"
flags=(--config "$root/.gitleaks.toml" --gitleaks-ignore-path "$work/empty.ignore"
       --ignore-gitleaks-allow --redact=100 --no-banner --no-color
       --max-archive-depth=2 --max-decode-depth=5)
"$scanner" dir "$work/source" "${flags[@]}"
if git -C "$root" rev-parse --verify HEAD >/dev/null 2>&1; then
  "$scanner" git "$root" --log-opts="--all --full-history" "${flags[@]}"
fi
