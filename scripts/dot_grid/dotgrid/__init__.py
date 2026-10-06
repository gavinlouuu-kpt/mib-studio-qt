"""Dot-grid wafer localization: codebook, mask pattern, renderer and reference decoder.

The pattern is an Anoto-style lattice of dots. Each dot is displaced from its
lattice node in one of four directions, encoding two bits (an x-plane bit and a
y-plane bit).  Column i of the x-plane holds a 63-period m-sequence cyclically
shifted by phi[i]; row j of the y-plane holds the same m-sequence shifted by
psi[j].  Phase differences between neighbouring columns (rows) form a random
symbol sequence whose short windows are unique, so any 6x6 dot window decodes
to an absolute lattice index. The registry (registry.py) holds every design
that carries a grid, one unique seed each, so a frame also identifies the
design. See docs/architecture/dot-grid-localization.md.
"""
import os as _os

from .codebook import (Codebook, generate_codebook, MNS_PERIOD, WINDOW_SYMBOLS, CODEC_CONTRACT,
                       CODEC_LINES, SUPPORTED_CODEC_CONTRACTS)
from .decode import decode_image, DecodeResult
from .render import render_view
from .registry import Design, Registry, decode_registry

# Core build version, one source with the C++ core (MIB_DOTGRID_CORE_VERSION).
with open(_os.path.join(_os.path.dirname(__file__), "VERSION"), encoding="utf-8") as _f:
    __version__ = _f.read().strip()


def core_identity() -> dict:
    """Identity of this (reference) codec core, recorded as encoder provenance."""
    return {"core_version": __version__, "contract": CODEC_CONTRACT, "line": CODEC_LINES[CODEC_CONTRACT],
            "source": "python-reference"}


__all__ = [
    "__version__", "core_identity", "CODEC_CONTRACT", "CODEC_LINES", "SUPPORTED_CODEC_CONTRACTS",
    "Codebook", "generate_codebook", "MNS_PERIOD", "WINDOW_SYMBOLS",
    "decode_image", "DecodeResult", "render_view",
    "Design", "Registry", "decode_registry",
]
