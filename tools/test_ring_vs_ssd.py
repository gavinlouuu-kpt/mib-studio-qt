#!/usr/bin/env python3
"""CI entry point for tools/ring_vs_ssd (qt-ci runs tools/test_*.py; CTest registers it as tools.ring_vs_ssd). Needs numpy only; exits 77 (skipped) without it."""
import sys
import unittest
from pathlib import Path

try:
    import numpy  # noqa: F401
except ImportError:
    print("numpy is not installed: tools/ring_vs_ssd tests skipped")
    sys.exit(77)

suite = unittest.defaultTestLoader.discover(str(Path(__file__).resolve().parent / "ring_vs_ssd"), pattern="test_ring_vs_ssd.py")
result = unittest.TextTestRunner(verbosity=1).run(suite)
sys.exit(0 if result.wasSuccessful() else 1)
