"""Dot-grid wafer localization: codebook, mask pattern, renderer and reference decoder.

The pattern is an Anoto-style lattice of dots. Each dot is displaced from its
lattice node in one of four directions, encoding two bits (an x-plane bit and a
y-plane bit).  Column i of the x-plane holds a 63-period m-sequence cyclically
shifted by phi[i]; row j of the y-plane holds the same m-sequence shifted by
psi[j].  Phase differences between neighbouring columns (rows) form a random
symbol sequence whose short windows are unique, so any 6x6 dot window decodes
to an absolute lattice index. See docs/architecture/dot-grid-localization.md.
"""
from .codebook import Codebook, generate_codebook, MNS_PERIOD, WINDOW_SYMBOLS
from .decode import decode_image, DecodeResult
from .render import render_view

__all__ = [
    "Codebook", "generate_codebook", "MNS_PERIOD", "WINDOW_SYMBOLS",
    "decode_image", "DecodeResult", "render_view",
]
