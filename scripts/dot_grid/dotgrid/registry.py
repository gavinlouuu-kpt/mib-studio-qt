"""Design registry: every chip design that carries a dot grid, keyed by a unique seed.

The codebook is a pure function of (seed, lattice size, geometry, origin), so a
registry entry stores only those parameters plus the chip (die) table; the app
regenerates each codebook at load time and decodes a frame against all of
them. A pattern generated from one seed does not decode under another (the
delta windows, cross-phase checks and the bit-agreement verification all have
to line up), so the seed doubles as the design identity: the decoder tells you
*which design* and *which chip* it is looking at, not only where.

Rules the registry enforces (``Registry.validate``):

- ids are unique slugs; seeds are unique across all designs, retired ones
  included (wafers of a retired design still exist on the bench);
- a seed is never reused: new designs get ``max(seed) + 1``;
- geometry must be fabricable (``2 * shift + dot < pitch``).

The C++ loader (``backend::dotgrid::Registry``) reads the same file; the
bundled copy is ``resources/defaults/dot_grid/registry.json``. See
docs/howto/dot-grid-mask-generation.md for the developer workflow.
"""
from __future__ import annotations

import datetime as _dt
import hashlib
import json
import re
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Sequence, Tuple

from .codebook import Chip, Codebook, generate_codebook

REGISTRY_VERSION = 1
STATUSES = ("active", "retired")
ID_PATTERN = re.compile(r"^[a-z0-9][a-z0-9._-]{0,63}$")


@dataclass
class Design:
    id: str
    name: str
    seed: int
    columns: int
    rows: int
    pitch_um: float
    dot_diameter_um: float
    displacement_um: float
    origin_um: Tuple[float, float] = (0.0, 0.0)
    revision: str = ""
    status: str = "active"
    design_scale: float = 1.0
    chips: List[Chip] = field(default_factory=list)
    keepout: Dict[str, float] = field(default_factory=dict)
    source: Dict[str, Optional[str]] = field(default_factory=dict)  # file, sha256
    author: str = ""
    registered: str = ""
    notes: str = ""

    def codebook(self) -> Codebook:
        return generate_codebook(self.seed, self.columns, self.rows, pitch_um=self.pitch_um,
                                 dot_diameter_um=self.dot_diameter_um,
                                 displacement_um=self.displacement_um, origin_um=tuple(self.origin_um),
                                 chips=self.chips, design_name=self.id, design_scale=self.design_scale)

    def to_dict(self) -> dict:
        d = {
            "id": self.id, "name": self.name, "revision": self.revision, "status": self.status,
            "seed": self.seed, "columns": self.columns, "rows": self.rows,
            "pitch_um": self.pitch_um, "dot_diameter_um": self.dot_diameter_um,
            "displacement_um": self.displacement_um, "origin_um": list(self.origin_um),
            "design_scale": self.design_scale, "keepout": dict(self.keepout),
            "source": dict(self.source), "author": self.author, "registered": self.registered,
            "notes": self.notes,
            "chips": [{"name": c.name, "x_min_um": c.x_min_um, "y_min_um": c.y_min_um,
                       "x_max_um": c.x_max_um, "y_max_um": c.y_max_um} for c in self.chips],
        }
        return d

    @classmethod
    def from_dict(cls, d: dict) -> "Design":
        return cls(id=d["id"], name=d.get("name", d["id"]), seed=int(d["seed"]),
                   columns=int(d["columns"]), rows=int(d["rows"]), pitch_um=float(d["pitch_um"]),
                   dot_diameter_um=float(d["dot_diameter_um"]),
                   displacement_um=float(d["displacement_um"]),
                   origin_um=tuple(d.get("origin_um", (0.0, 0.0))), revision=d.get("revision", ""),
                   status=d.get("status", "active"), design_scale=float(d.get("design_scale", 1.0)),
                   chips=[Chip(**c) for c in d.get("chips", [])], keepout=dict(d.get("keepout", {})),
                   source=dict(d.get("source", {})), author=d.get("author", ""),
                   registered=d.get("registered", ""), notes=d.get("notes", ""))

    @classmethod
    def from_codebook(cls, cb: Codebook, design_id: str, name: str, **meta) -> "Design":
        return cls(id=design_id, name=name, seed=cb.seed, columns=cb.columns, rows=cb.rows,
                   pitch_um=cb.pitch_um, dot_diameter_um=cb.dot_diameter_um,
                   displacement_um=cb.displacement_um, origin_um=tuple(cb.origin_um),
                   design_scale=cb.design_scale, chips=list(cb.chips), **meta)


class Registry:
    def __init__(self, designs: Sequence[Design] = ()) -> None:
        self.designs: List[Design] = list(designs)
        self._codebooks: Dict[str, Codebook] = {}

    # ----- io -----
    @classmethod
    def load(cls, path: str) -> "Registry":
        with open(path, "r", encoding="utf-8") as f:
            d = json.load(f)
        if d.get("version") != REGISTRY_VERSION:
            raise ValueError(f"unsupported registry version {d.get('version')}")
        reg = cls(Design.from_dict(x) for x in d.get("designs", []))
        errors = reg.validate()
        if errors:
            raise ValueError("invalid registry: " + "; ".join(errors))
        return reg

    def save(self, path: str) -> None:
        errors = self.validate()
        if errors:
            raise ValueError("refusing to save an invalid registry: " + "; ".join(errors))
        doc = {"version": REGISTRY_VERSION,
               "designs": [x.to_dict() for x in sorted(self.designs, key=lambda x: x.seed)]}
        with open(path, "w", encoding="utf-8") as f:
            json.dump(doc, f, indent=1)
            f.write("\n")

    # ----- queries -----
    def find(self, design_id: str) -> Optional[Design]:
        return next((x for x in self.designs if x.id == design_id), None)

    def find_source(self, sha256: str) -> Optional[Design]:
        return next((x for x in self.designs if x.source.get("sha256") == sha256), None)

    def codebook(self, design_id: str) -> Codebook:
        if design_id not in self._codebooks:
            design = self.find(design_id)
            if design is None:
                raise KeyError(f"design '{design_id}' is not registered")
            self._codebooks[design_id] = design.codebook()
        return self._codebooks[design_id]

    def next_seed(self) -> int:
        """Seeds are never reused: one past the highest ever registered."""
        return max((x.seed for x in self.designs), default=0) + 1

    # ----- mutation -----
    def add(self, design: Design) -> None:
        trial = Registry(self.designs + [design])
        errors = trial.validate()
        if errors:
            raise ValueError("; ".join(errors))
        self.designs.append(design)

    def validate(self) -> List[str]:
        errors: List[str] = []
        ids: Dict[str, int] = {}
        seeds: Dict[int, str] = {}
        for x in self.designs:
            if not ID_PATTERN.match(x.id):
                errors.append(f"id '{x.id}' must be a lowercase slug ([a-z0-9._-], max 64)")
            if x.id in ids:
                errors.append(f"duplicate id '{x.id}'")
            ids[x.id] = x.seed
            if x.seed in seeds:
                errors.append(f"seed {x.seed} used by both '{seeds[x.seed]}' and '{x.id}'")
            seeds[x.seed] = x.id
            if x.seed < 0:
                errors.append(f"'{x.id}': seed must be non-negative")
            if x.status not in STATUSES:
                errors.append(f"'{x.id}': status must be one of {STATUSES}")
            if x.columns < 3 or x.rows < 3:
                errors.append(f"'{x.id}': lattice too small")
            if x.displacement_um * 2 + x.dot_diameter_um >= x.pitch_um:
                errors.append(f"'{x.id}': dots would touch (2*shift + dot >= pitch)")
        return errors


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def today() -> str:
    return _dt.date.today().isoformat()


# ---------------------------------------------------------------- multi-design decode
def decode_registry(registry: Registry, image, um_per_px_hint: float, *, min_votes: int = 3):
    """Decode a frame against every registered design. Mirrors backend::dotgrid::Decoder.

    Returns (DecodeResult, design_id). Exactly one design must decode; two or more is reported
    as ``ambiguous design`` and never as a pose.
    """
    from .decode import DecodeResult, decode_image
    hits = []
    best_fail = None
    for x in registry.designs:
        r = decode_image(registry.codebook(x.id), image, um_per_px_hint, min_votes=min_votes)
        if r.ok:
            hits.append((r, x.id))
        elif best_fail is None or r.votes > best_fail[0].votes:
            best_fail = (r, None)
    if len(hits) == 1:
        return hits[0]
    if len(hits) > 1:
        names = ", ".join(i for _, i in hits)
        return DecodeResult(False, f"ambiguous design ({names})", dots=hits[0][0].dots), None
    return best_fail if best_fail is not None else (DecodeResult(False, "empty registry"), None)


def cross_check(registry: Registry, design_id: str, *, views: int = 6, seed: int = 0,
                um_per_px: float = 0.293, progress=None) -> List[str]:
    """Synthetic discrimination check run when a design is registered.

    Renders ``views`` frames of ``design_id`` at random poses inside its chips (or anywhere on
    the lattice when it has no chip table) and requires every one to decode as that design
    with the right position, and renders the same number of frames of every other design and
    requires that none decodes as ``design_id``. Returns a list of problems (empty = pass).
    """
    import numpy as np
    from .render import ViewPose, render_view
    rng = np.random.default_rng(seed)
    problems: List[str] = []

    def random_pose(d: Design) -> ViewPose:
        if d.chips:
            c = d.chips[int(rng.integers(len(d.chips)))]
            x = float(rng.uniform(c.x_min_um + 500, c.x_max_um - 500))
            y = float(rng.uniform(c.y_min_um + 500, c.y_max_um - 500))
        else:
            x0, y0 = d.origin_um
            x = float(x0 + rng.uniform(0.2, 0.8) * d.columns * d.pitch_um)
            y = float(y0 + rng.uniform(0.2, 0.8) * d.rows * d.pitch_um)
        return ViewPose(centre_um=(x, y), theta_deg=float(rng.uniform(-180, 180)), um_per_px=um_per_px,
                        mirrored=bool(rng.integers(2)))

    for d in registry.designs:
        for k in range(views):
            pose = random_pose(d)
            img = render_view(registry.codebook(d.id), pose, seed=int(rng.integers(1 << 30)))
            r, got = decode_registry(registry, img, um_per_px * 1.05)
            if d.id == design_id:
                if got != design_id:
                    problems.append(f"view {k} of '{design_id}' decoded as {got!r} ({r.reason})")
                elif max(abs(r.centre_um[0] - pose.centre_um[0]), abs(r.centre_um[1] - pose.centre_um[1])) > 2.0:
                    problems.append(f"view {k} of '{design_id}' decoded at the wrong position")
            elif got == design_id:
                problems.append(f"view {k} of '{d.id}' was mistaken for '{design_id}'")
        if progress:
            progress(f"cross-check: {d.id} ok" if not problems else f"cross-check: {d.id}: {len(problems)} problem(s)")
    return problems
