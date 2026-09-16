"""Pure, offline analysis and conservative plateau selection."""

from dataclasses import dataclass
import csv
import json
import math
from pathlib import Path
import statistics


@dataclass(frozen=True)
class Policy:
    max_cv_percent: float = 1.0
    min_mean: float = 20.0
    max_mean: float = 235.0
    max_clipped_fraction: float = 0.001
    max_reader_missing_fraction: float = 0.01
    min_frames: int = 200
    min_plateau_points: int = 3
    near_best_ratio: float = 1.5
    near_best_margin_percent: float = 0.15
    min_rate_ratio: float = 0.9

    def __post_init__(self):
        if any(not math.isfinite(v) for v in vars(self).values()):
            raise ValueError("Policy values must be finite")
        if not (0 < self.max_cv_percent <= 100 and 0 < self.min_mean < self.max_mean < 250):
            raise ValueError("Invalid CV/brightness policy")
        if not (0 <= self.max_clipped_fraction < 1 and 0 <= self.max_reader_missing_fraction < 1):
            raise ValueError("Invalid fraction policy")
        if not (isinstance(self.min_frames, int) and self.min_frames >= 2 and
                isinstance(self.min_plateau_points, int) and self.min_plateau_points >= 3):
            raise ValueError("Require at least two frames and three plateau points")
        if self.near_best_ratio < 1 or self.near_best_margin_percent < 0 or not 0 < self.min_rate_ratio <= 1:
            raise ValueError("Invalid selection policy")


def percentile(ordered, fraction):
    position = (len(ordered) - 1) * fraction
    low = int(position)
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


def analyze_capture(directory: Path, policy: Policy = Policy()) -> dict:
    """Analyze sampler output; reject corrupt, dark, clipped or incomplete data."""
    directory = Path(directory)
    meta = json.loads((directory / "capture.json").read_text(encoding="utf-8"))
    if meta.get("schema_version") != 1 or meta.get("ok") is not True or meta.get("shutdown_confirmed") is not True:
        raise ValueError(f"Capture failed or shutdown unconfirmed: {directory}")
    values, bands, clipping = [], [[], [], []], []
    previous_index = previous_time = -1
    with (directory / "frames.csv").open(newline="", encoding="utf-8") as stream:
        for row in csv.DictReader(stream):
            index, timestamp = int(row["index"]), int(row["host_us"])
            if index <= previous_index or timestamp < previous_time or timestamp <= 0:
                raise ValueError("Repeated/out-of-order frame identity or acquisition timestamp")
            previous_index, previous_time = index, timestamp
            numbers = [float(row[key]) for key in ("mean", "top", "middle", "bottom", "clipped_fraction")]
            if any(not math.isfinite(v) for v in numbers) or any(not 0 <= v <= 255 for v in numbers[:4]) or not 0 <= numbers[4] <= 1:
                raise ValueError("Invalid intensity sample")
            values.append(numbers[0])
            for band, value in zip(bands, numbers[1:4]):
                band.append(value)
            clipping.append(numbers[4])
    count = len(values)
    if count != meta.get("frames") or count < 2:
        raise ValueError("Missing or truncated frame data")
    missing, excluded = meta.get("reader_missing"), meta.get("reader_excluded")
    if any(type(v) is not int or v < 0 for v in (missing, excluded, meta.get("reader_considered"))):
        raise ValueError("Invalid frame accounting")
    if meta["reader_considered"] != count + missing + excluded:
        raise ValueError("Frame accounting is not conserved")
    ordered = sorted(values)
    mean = statistics.fmean(values)
    sd = statistics.pstdev(values)
    cv = 100 * sd / mean if mean > 0 else 1e6
    band_cv = [100 * statistics.pstdev(b) / statistics.fmean(b) if statistics.fmean(b) > 0 else 1e6 for b in bands]
    fps = float(meta["capture_fps"])
    if not math.isfinite(fps) or fps <= 0:
        raise ValueError("Invalid capture rate")
    measured_span = (previous_time - _first_timestamp(directory)) / 1e6
    seconds = float(meta["seconds"])
    if not math.isfinite(seconds) or seconds <= 0:
        raise ValueError("Invalid measurement duration")
    missing_fraction = missing / (count + missing)
    reasons = []
    if count < policy.min_frames: reasons.append("too few measured frames")
    if measured_span < seconds * 0.9: reasons.append("measurement covers less than 90% of requested duration")
    if min(values) < policy.min_mean: reasons.append("dark frames")
    if max(values) > policy.max_mean: reasons.append("image too bright")
    if max(clipping) > policy.max_clipped_fraction: reasons.append("clipped pixels")
    if cv > policy.max_cv_percent or max(band_cv) > policy.max_cv_percent:
        reasons.append("brightness variation exceeds limit")
    if missing_fraction > policy.max_reader_missing_fraction: reasons.append("too many reader misses")
    warnings = []
    for name in ("transport_lost", "discarded"):
        metric = meta.get(name, {})
        if metric.get("validity") == "valid":
            if type(metric.get("value")) is not int or metric["value"] < 0:
                raise ValueError("Invalid transport accounting")
            if metric["value"]: reasons.append(f"{name}: {metric['value']}")
        else:
            warnings.append(f"{name} is {metric.get('validity', 'unavailable')}; zero loss is not established")
    return dict(directory=str(directory), frames=count, mean=mean, sd=sd, cv_percent=cv,
                band_cv_percent=band_cv, score=max(cv, *band_cv), p01=percentile(ordered, .01),
                p50=percentile(ordered, .5), p99=percentile(ordered, .99),
                max_clipped_fraction=max(clipping), reader_missing=missing,
                reader_missing_fraction=missing_fraction, capture_fps=fps,
                measured_seconds=measured_span, accepted=not reasons, reasons=reasons, warnings=warnings)


def _first_timestamp(directory):
    with (directory / "frames.csv").open(newline="", encoding="utf-8") as stream:
        return int(next(csv.DictReader(stream))["host_us"])


def select_candidate(candidates: list[dict], policy: Policy = Policy()) -> dict:
    """Return the center of a near-best contiguous tested delay interval.

    Candidates must contain exposure_us, delay_us, and both analyzed modes.
    Rejected grid points split intervals; a single lucky minimum cannot win.
    """
    if not candidates:
        raise ValueError("No candidates")
    seen = set()
    for candidate in candidates:
        key = candidate["exposure_us"], candidate["delay_us"]
        if key in seen: raise ValueError("Duplicate candidate")
        seen.add(key)
        modes = candidate.get("modes", {})
        if set(modes) != {"overview", "experiment"}:
            raise ValueError("Both camera modes are required")
        if any(not math.isfinite(m["score"]) or m["score"] < 0 for m in modes.values()):
            raise ValueError("Invalid candidate score")
    good = [c for c in candidates if all(m["accepted"] for m in c["modes"].values())]
    if not good: raise ValueError("No unclipped, sufficiently bright and stable candidate")
    score = lambda c: max(m["score"] for m in c["modes"].values())
    best = min(map(score, good))
    cutoff = max(best * policy.near_best_ratio, best + policy.near_best_margin_percent)
    intervals = []
    for exposure in sorted({c["exposure_us"] for c in candidates}):
        interval = []
        ordered = sorted((c for c in candidates if c["exposure_us"] == exposure), key=lambda c: c["delay_us"])
        for candidate in ordered + [None]:
            if candidate is not None and all(m["accepted"] for m in candidate["modes"].values()) and score(candidate) <= cutoff:
                interval.append(candidate)
            else:
                if len(interval) >= policy.min_plateau_points: intervals.append(interval)
                interval = []
    if not intervals: raise ValueError("No stable delay interval; expand/refine the sweep (at least three neighboring points)")
    def central(interval):
        midpoint = (interval[0]["delay_us"] + interval[-1]["delay_us"]) / 2
        return min(interval, key=lambda c: (abs(c["delay_us"] - midpoint), score(c), c["delay_us"]))
    chosen = min(intervals, key=lambda group: (score(central(group)), -(group[-1]["delay_us"] - group[0]["delay_us"])))
    result = dict(central(chosen))
    result["plateau_delays_us"] = [c["delay_us"] for c in chosen]
    return result
