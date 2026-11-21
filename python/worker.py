#!/usr/bin/env python3
"""Entry point usable with Python -I, without installing the package."""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mam_tools.cli import main

if __name__ == "__main__":
    raise SystemExit(main())
