#!/usr/bin/env python3
"""CI entry point for tools/ring_vs_ssd/rehearsal (the fake pzrec and the golden frame content of the host rehearsal provider). POSIX with numpy; elsewhere it passes without running."""
import subprocess
import sys
from pathlib import Path

if sys.platform == "win32":
    print("tools/ring_vs_ssd/rehearsal: the fake pzrec is POSIX only: not run")
    sys.exit(0)
try:
    import numpy  # noqa: F401
except ImportError:
    print("numpy is not installed: tools/ring_vs_ssd/rehearsal tests skipped")
    sys.exit(0)
sys.exit(subprocess.run([sys.executable, str(Path(__file__).resolve().parent / "ring_vs_ssd" / "rehearsal" / "test_rehearsal.py")]).returncode)
