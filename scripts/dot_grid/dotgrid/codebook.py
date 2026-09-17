"""Codebook generation and lookup (pure Python + numpy, no OpenCV)."""
from __future__ import annotations

import json
from dataclasses import dataclass, field, asdict
from typing import Dict, List, Optional, Sequence, Tuple

import numpy as np

MNS_ORDER = 6
MNS_PERIOD = (1 << MNS_ORDER) - 1  # 63
WINDOW_SYMBOLS = 2                 # phase differences per lookup -> 3 columns/rows
WINDOW_DOTS = WINDOW_SYMBOLS + 1   # lines needed along the coded axis
CODEBOOK_VERSION = 1

# Direction encoding for the two bits (x_bit, y_bit) -> unit displacement in lattice units.
DIRECTIONS = {(0, 0): (1, 0), (1, 0): (-1, 0), (0, 1): (0, 1), (1, 1): (0, -1)}
DIRECTION_TO_BITS = {v: k for k, v in DIRECTIONS.items()}


def m_sequence(order: int = MNS_ORDER) -> List[int]:
    """Maximal-length LFSR sequence, x^6 + x + 1 (primitive). Verified for uniqueness of windows."""
    assert order == 6, "only order 6 is wired up"
    state = [1, 0, 0, 0, 0, 0]
    seq: List[int] = []
    for _ in range(MNS_PERIOD):
        seq.append(state[-1])
        new = state[-1] ^ state[-2]  # taps for x^6 + x^5 + 1 (primitive)
        state = [new] + state[:-1]
    windows = {tuple(seq[(k + t) % MNS_PERIOD] for t in range(order)) for k in range(MNS_PERIOD)}
    assert len(windows) == MNS_PERIOD, "LFSR taps are not primitive"
    return seq


def _splitmix64(x: int) -> int:
    x = (x + 0x9E3779B97F4A7C15) & 0xFFFFFFFFFFFFFFFF
    z = x
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & 0xFFFFFFFFFFFFFFFF
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & 0xFFFFFFFFFFFFFFFF
    return z ^ (z >> 31)


def _delta_sequence(seed: int, axis: int, count: int) -> List[int]:
    """Deterministic symbol sequence in [0, 62] whose WINDOW_SYMBOLS-windows are unique.

    The generator is portable (SplitMix64 on (seed, axis, index, attempt)) so the C++
    side can regenerate the same codebook from the seed alone.
    """
    deltas: List[int] = []
    seen: Dict[Tuple[int, ...], int] = {}
    attempt = 0
    i = 0
    while i < count:
        h = _splitmix64(((seed * 0x1000193) & 0xFFFFFFFFFFFFFFFF) ^ (axis << 56) ^ (i << 20) ^ attempt)
        d = h % MNS_PERIOD
        deltas.append(d)
        if i + 1 >= WINDOW_SYMBOLS:
            w = tuple(deltas[i + 1 - WINDOW_SYMBOLS: i + 1])
            if w in seen:
                deltas.pop()
                attempt += 1
                if attempt > 1000:
                    raise RuntimeError("could not build a unique delta sequence")
                continue
            seen[w] = i + 1 - WINDOW_SYMBOLS
        attempt = 0
        i += 1
    return deltas


def _phases_from_deltas(deltas: Sequence[int]) -> List[int]:
    phases = [0]
    for d in deltas:
        phases.append((phases[-1] + d) % MNS_PERIOD)
    return phases


@dataclass
class Chip:
    name: str
    x_min_um: float
    y_min_um: float
    x_max_um: float
    y_max_um: float


@dataclass
class Codebook:
    seed: int
    pitch_um: float
    dot_diameter_um: float
    displacement_um: float
    columns: int
    rows: int
    origin_um: Tuple[float, float]
    mns: List[int]
    phi: List[int]
    psi: List[int]
    chips: List[Chip] = field(default_factory=list)
    version: int = CODEBOOK_VERSION
    design_name: str = ""
    design_scale: float = 1.0

    # ----- derived lookups -----
    def __post_init__(self) -> None:
        self._mns_index = {tuple(self.mns[(k + t) % MNS_PERIOD] for t in range(MNS_ORDER)): k
                           for k in range(MNS_PERIOD)}
        self._col_lookup = self._build_lookup(self.phi)
        self._row_lookup = self._build_lookup(self.psi)

    @staticmethod
    def _build_lookup(phases: Sequence[int]) -> Dict[Tuple[int, ...], int]:
        deltas = [(phases[i + 1] - phases[i]) % MNS_PERIOD for i in range(len(phases) - 1)]
        table: Dict[Tuple[int, ...], int] = {}
        for i in range(len(deltas) - WINDOW_SYMBOLS + 1):
            w = tuple(deltas[i:i + WINDOW_SYMBOLS])
            assert w not in table, "delta windows must be unique"
            table[w] = i
        return table

    def mns_phase(self, bits: Sequence[int]) -> Optional[int]:
        """Index k such that bits == mns[k:k+6] (cyclic); None if not a valid window."""
        return self._mns_index.get(tuple(int(b) for b in bits))

    def lookup_column(self, deltas: Sequence[int]) -> Optional[int]:
        return self._col_lookup.get(tuple(deltas))

    def lookup_row(self, deltas: Sequence[int]) -> Optional[int]:
        return self._row_lookup.get(tuple(deltas))

    # ----- pattern access -----
    def bits(self, i: int, j: int) -> Tuple[int, int]:
        """(x_bit, y_bit) of lattice node column i, row j."""
        return (self.mns[(j + self.phi[i]) % MNS_PERIOD], self.mns[(i + self.psi[j]) % MNS_PERIOD])

    def direction(self, i: int, j: int) -> Tuple[int, int]:
        return DIRECTIONS[self.bits(i, j)]

    def node_um(self, i: int, j: int) -> Tuple[float, float]:
        return (self.origin_um[0] + i * self.pitch_um, self.origin_um[1] + j * self.pitch_um)

    def dot_um(self, i: int, j: int) -> Tuple[float, float]:
        dx, dy = self.direction(i, j)
        x, y = self.node_um(i, j)
        return (x + dx * self.displacement_um, y + dy * self.displacement_um)

    def chip_at(self, x_um: float, y_um: float) -> Optional[Chip]:
        for c in self.chips:
            if c.x_min_um <= x_um <= c.x_max_um and c.y_min_um <= y_um <= c.y_max_um:
                return c
        return None

    # ----- serialisation -----
    def to_dict(self) -> dict:
        d = asdict(self)
        d["origin_um"] = list(self.origin_um)
        return d

    def save(self, path: str) -> None:
        with open(path, "w", encoding="utf-8") as f:
            json.dump(self.to_dict(), f, indent=1)

    @classmethod
    def load(cls, path: str) -> "Codebook":
        with open(path, "r", encoding="utf-8") as f:
            d = json.load(f)
        if d.get("version") != CODEBOOK_VERSION:
            raise ValueError(f"unsupported codebook version {d.get('version')}")
        d["chips"] = [Chip(**c) for c in d.get("chips", [])]
        d["origin_um"] = tuple(d["origin_um"])
        return cls(**d)


def generate_codebook(seed: int, columns: int, rows: int, *, pitch_um: float = 50.0,
                      dot_diameter_um: float = 12.0, displacement_um: float = 8.0,
                      origin_um: Tuple[float, float] = (0.0, 0.0), chips: Sequence[Chip] = (),
                      design_name: str = "", design_scale: float = 1.0) -> Codebook:
    if displacement_um * 2 + dot_diameter_um >= pitch_um:
        raise ValueError("dots would touch: 2*displacement + diameter must be < pitch")
    mns = m_sequence()
    phi = _phases_from_deltas(_delta_sequence(seed, 0, columns - 1))
    psi = _phases_from_deltas(_delta_sequence(seed, 1, rows - 1))
    return Codebook(seed=seed, pitch_um=pitch_um, dot_diameter_um=dot_diameter_um,
                    displacement_um=displacement_um, columns=columns, rows=rows,
                    origin_um=origin_um, mns=mns, phi=phi, psi=psi, chips=list(chips),
                    design_name=design_name, design_scale=design_scale)
