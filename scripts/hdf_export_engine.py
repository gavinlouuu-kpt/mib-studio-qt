"""
Bounded-memory HDF5 export engine shared by the CLI (``export_hdf5.py``) and
the PySide GUI (``export_worker.py`` / ``hdf5_export_app.py``).

Issue #344 contract:

* Images are streamed one frame at a time by integer index. The complete
  image dataset is never materialized (``dataset[:]`` / ``np.array(dataset)``
  are never used for image payloads), so peak image memory is independent of
  the number of frames.
* Every export is an immutable :class:`ExportJob` with a job id; the engine
  reports :class:`ExportProgress` events and returns exactly one
  :class:`ExportResult` with an explicit terminal state.
* Cancellation is a ``threading.Event`` checked before opening the source,
  before each metadata/metrics artifact, before every image, before every
  series record and before committing.
* Output is transactional: metrics-only jobs write ``.<name>.partial-<job>``
  next to the final file, image/all jobs write into
  ``.<name>.partial-<job>/``; the final name is published by a rename only
  after success. A cancelled/failed job removes its partial output (or, when
  removal fails, leaves it under the ``.partial-`` name with an
  ``export-failure.json`` manifest). A normal-looking directory can never
  contain a partial export.
* Generated names are chosen from one directory listing (``max suffix + 1``)
  instead of probing ``_2``, ``_3``, ... with ``exists()`` per candidate.

This module contains no PySide imports.
"""

from __future__ import annotations

import enum
import json
import os
import re
import secrets
import shutil
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Dict, Iterable, Optional, Tuple

import export_hdf5 as core


# ---------------------------------------------------------------------------
# Contracts
# ---------------------------------------------------------------------------


class ExportFormat(str, enum.Enum):
    CSV = "csv"
    JSON = "json"
    IMAGES = "images"
    ALL = "all"

    @property
    def writes_images(self) -> bool:
        return self in (ExportFormat.IMAGES, ExportFormat.ALL)

    @property
    def writes_csv(self) -> bool:
        return self in (ExportFormat.CSV, ExportFormat.ALL)


class FrameSelection(str, enum.Enum):
    VALID = "valid"
    INVALID = "invalid"
    BOTH = "both"

    @property
    def includes_valid(self) -> bool:
        return self in (FrameSelection.VALID, FrameSelection.BOTH)

    @property
    def includes_invalid(self) -> bool:
        return self in (FrameSelection.INVALID, FrameSelection.BOTH)


class ExportPhase(str, enum.Enum):
    VALIDATING = "validating"
    METADATA = "metadata"
    METRICS = "metrics"
    VALID_IMAGES = "valid_images"
    INVALID_IMAGES = "invalid_images"
    SERIES_IMAGES = "series_images"
    COMMITTING = "committing"
    CLEANUP = "cleanup"


class ExportState(str, enum.Enum):
    COMPLETED = "completed"
    CANCELLED = "cancelled"
    FAILED = "failed"


class ExportCancelled(Exception):
    """Raised inside the engine when the cancellation event is set."""


class ExportFailed(Exception):
    """Raised inside the engine for a structured, user-facing failure."""


def new_job_id() -> str:
    """Unique, sortable job identifier (``<unix-ms>-<hex>``)."""
    return f"{int(time.time() * 1000):x}-{secrets.token_hex(3)}"


@dataclass(frozen=True)
class ExportJob:
    input_path: Path
    output_root: Path
    format: ExportFormat = ExportFormat.CSV
    frame_selection: FrameSelection = FrameSelection.BOTH
    pixel_to_micron: float = 0.4886
    # Series images: None exports every series frame; (start, end) is an
    # inclusive zero-based range; export_series=False skips the dataset.
    export_series: bool = True
    series_range: Optional[Tuple[int, int]] = None
    # Keep a visible ``.partial-<job>`` output (with a failure manifest)
    # instead of deleting it when the job does not complete.
    keep_partial_on_failure: bool = False
    job_id: str = field(default_factory=new_job_id)

    def __post_init__(self) -> None:
        # Normalize loosely-typed callers (CLI strings) into the enums and
        # keep paths as Path objects; frozen dataclass -> object.__setattr__.
        object.__setattr__(self, "input_path", Path(self.input_path))
        object.__setattr__(self, "output_root", Path(self.output_root))
        object.__setattr__(self, "format", ExportFormat(self.format))
        object.__setattr__(self, "frame_selection", FrameSelection(self.frame_selection))
        if self.series_range is not None:
            start, end = self.series_range
            if start < 0 or end < start:
                raise ValueError(f"invalid series range {self.series_range}")
            object.__setattr__(self, "series_range", (int(start), int(end)))


@dataclass(frozen=True)
class ExportProgress:
    job_id: str
    phase: ExportPhase
    completed: int
    total: int
    current_output: str = ""
    message: str = ""


@dataclass(frozen=True)
class ExportResult:
    job_id: str
    state: ExportState
    final_path: Optional[Path]
    valid_count: int = 0
    invalid_count: int = 0
    images_exported: int = 0
    images_skipped: int = 0
    series_exported: int = 0
    duration_s: float = 0.0
    warnings: Tuple[str, ...] = ()
    error: str = ""
    partial_path: Optional[Path] = None  # retained partial output, if any
    experiment_info: Dict[str, Any] = field(default_factory=dict)

    @property
    def completed(self) -> bool:
        return self.state == ExportState.COMPLETED

    def summary(self) -> str:
        if self.state == ExportState.COMPLETED:
            return (
                f"Export {self.job_id} complete: valid={self.valid_count} invalid={self.invalid_count} "
                f"images={self.images_exported} series={self.series_exported} -> {self.final_path}"
            )
        if self.state == ExportState.CANCELLED:
            return f"Export {self.job_id} cancelled: {self.error or 'cancelled by user'}"
        return f"Export {self.job_id} failed: {self.error}"


ProgressCallback = Callable[[ExportProgress], None]
ImageWriter = Callable[[Any, Path, int], bool]


# ---------------------------------------------------------------------------
# Bounded generated-name lookup
# ---------------------------------------------------------------------------

_NUMBERED_PLACEHOLDER = "{suffix}"


def _pattern_regex(numbered_pattern: str) -> re.Pattern:
    head, tail = numbered_pattern.split(_NUMBERED_PLACEHOLDER, 1)
    return re.compile(rf"^{re.escape(head)}(\d+){re.escape(tail)}$")


def next_available_name(parent: Path, first_name: str, numbered_pattern: str,
                        existing_names: Optional[Iterable[str]] = None) -> Path:
    """Pick the next unused generated name from a single listing of ``parent``.

    Returns ``first_name`` when unused; otherwise ``max(existing suffix) + 1``
    (minimum ``_2``). Cost is one directory scan regardless of how many prior
    exports live in ``parent``.
    """
    if existing_names is None:
        try:
            with os.scandir(parent) as it:
                names = [entry.name for entry in it]
        except FileNotFoundError:
            names = []
    else:
        names = list(existing_names)
    present = set(names)
    if first_name not in present:
        return parent / first_name
    regex = _pattern_regex(numbered_pattern)
    max_suffix = 1
    for name in names:
        match = regex.match(name)
        if match:
            try:
                max_suffix = max(max_suffix, int(match.group(1)))
            except ValueError:
                continue
    suffix = max_suffix + 1
    candidate = parent / numbered_pattern.format(suffix=suffix)
    # Defensive: a numbered name can exist without matching the regex only if
    # it was renamed by hand; keep the loop bounded and cheap.
    for _ in range(1000):
        if candidate.name not in present:
            return candidate
        suffix += 1
        candidate = parent / numbered_pattern.format(suffix=suffix)
    return candidate


def partial_name_for(final_name: str, job_id: str) -> str:
    return f".{final_name}.partial-{job_id}"


# ---------------------------------------------------------------------------
# Streaming helpers
# ---------------------------------------------------------------------------


def dataset_length(dataset: Any) -> int:
    shape = getattr(dataset, "shape", None)
    if shape:
        return int(shape[0])
    try:
        return len(dataset)
    except TypeError:
        return 0


def write_frame_image(image: Any, destination: Path, index: int) -> bool:
    """Prepare one frame and write it as an uncompressed TIFF.

    Returns False when the frame has an unsupported shape (skipped with a
    warning, matching the historical exporter). A write failure is a job
    failure: the output would otherwise look complete while missing frames.
    """
    prepared = core._prepare_image_for_tiff(image, index)
    if prepared is None:
        return False
    if not core.cv2.imwrite(str(destination), prepared):
        raise ExportFailed(f"failed to write image {destination}")
    return True


def _check_cancel(cancel_event: Optional[threading.Event]) -> None:
    if cancel_event is not None and cancel_event.is_set():
        raise ExportCancelled()


def _atomic_replace(src: Path, dst: Path) -> None:
    os.replace(src, dst)


def _write_failure_manifest(partial: Path, job: ExportJob, state: ExportState, error: str,
                            counts: Dict[str, int]) -> None:
    manifest = {
        "job_id": job.job_id,
        "state": state.value,
        "error": error,
        "input": str(job.input_path),
        "format": job.format.value,
        "frame_selection": job.frame_selection.value,
        "counts": counts,
        "note": "This export did not complete; contents are partial.",
    }
    try:
        if partial.is_dir():
            (partial / "export-failure.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        else:
            partial.with_name(partial.name + ".failure.json").write_text(
                json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    except OSError:
        pass


def _discard_partial(partial: Optional[Path], job: ExportJob, state: ExportState, error: str,
                     counts: Dict[str, int]) -> Optional[Path]:
    """Remove partial output; return the retained path when it must stay."""
    if partial is None or not partial.exists():
        return None
    if job.keep_partial_on_failure:
        _write_failure_manifest(partial, job, state, error, counts)
        return partial
    try:
        if partial.is_dir():
            shutil.rmtree(partial)
        else:
            partial.unlink()
        return None
    except OSError:
        _write_failure_manifest(partial, job, state, error, counts)
        return partial


# ---------------------------------------------------------------------------
# Engine
# ---------------------------------------------------------------------------


class _Run:
    """One execution of an ExportJob (mutable bookkeeping lives here)."""

    def __init__(self, job: ExportJob, cancel_event: Optional[threading.Event],
                 on_progress: Optional[ProgressCallback], image_writer: ImageWriter) -> None:
        self.job = job
        self.cancel_event = cancel_event
        self.on_progress = on_progress
        self.image_writer = image_writer
        self.warnings: list[str] = []
        self.valid_count = 0
        self.invalid_count = 0
        self.images_exported = 0
        self.images_skipped = 0
        self.series_exported = 0
        self.experiment_info: Dict[str, Any] = {}
        self.partial: Optional[Path] = None
        self.final_path: Optional[Path] = None
        self.total_units = 0
        self.completed_units = 0

    # -- reporting ---------------------------------------------------------
    def progress(self, phase: ExportPhase, current: str = "", message: str = "") -> None:
        if self.on_progress is None:
            return
        self.on_progress(ExportProgress(self.job.job_id, phase, self.completed_units,
                                        self.total_units, current, message))

    def warn(self, message: str) -> None:
        self.warnings.append(message)
        self.progress(ExportPhase.METADATA, message=f"WARNING: {message}")

    def counts(self) -> Dict[str, int]:
        return {
            "valid": self.valid_count,
            "invalid": self.invalid_count,
            "images_exported": self.images_exported,
            "images_skipped": self.images_skipped,
            "series_exported": self.series_exported,
        }

    # -- destination -------------------------------------------------------
    def choose_destination(self) -> Tuple[Path, Path]:
        """Return (final_path, partial_path); creates the partial output."""
        job = self.job
        base = core.source_base_name(job.input_path)
        if job.format == ExportFormat.CSV:
            final = next_available_name(job.output_root, f"{base}_metrics.csv", f"{base}_metrics_{{suffix}}.csv")
        elif job.format == ExportFormat.JSON:
            final = next_available_name(job.output_root, f"{base}_metrics.json", f"{base}_metrics_{{suffix}}.json")
        else:
            final = next_available_name(job.output_root, base, f"{base}_{{suffix}}")
        partial = final.with_name(partial_name_for(final.name, job.job_id))
        if job.format.writes_images:
            partial.mkdir(parents=True, exist_ok=False)
        return final, partial

    def commit(self) -> Path:
        """Publish the partial output under its final name (atomic rename)."""
        assert self.partial is not None and self.final_path is not None
        job = self.job
        final = self.final_path
        base = core.source_base_name(job.input_path)
        for _ in range(1000):
            try:
                if final.exists():
                    raise FileExistsError(final)
                _atomic_replace(self.partial, final)
                return final
            except (FileExistsError, OSError) as exc:
                # A concurrent exporter took the name: pick the next one.
                if isinstance(exc, OSError) and not isinstance(exc, FileExistsError) and \
                        getattr(exc, "errno", None) not in (17, 39, 66):  # EEXIST/ENOTEMPTY/ENOTEMPTY-mac
                    raise
                if job.format == ExportFormat.CSV:
                    final = next_available_name(job.output_root, f"{base}_metrics.csv", f"{base}_metrics_{{suffix}}.csv")
                elif job.format == ExportFormat.JSON:
                    final = next_available_name(job.output_root, f"{base}_metrics.json", f"{base}_metrics_{{suffix}}.json")
                else:
                    final = next_available_name(job.output_root, base, f"{base}_{{suffix}}")
        raise ExportFailed(f"could not publish export under {self.final_path.parent}")

    # -- phases ------------------------------------------------------------
    def export_metrics(self, metadata_valid: Any, metadata_invalid: Any, target: Path) -> None:
        job = self.job
        _check_cancel(self.cancel_event)
        self.progress(ExportPhase.METRICS, str(target), "Exporting metrics...")
        if job.format == ExportFormat.JSON:
            self.valid_count, self.invalid_count = core.export_metrics_to_json(
                metadata_valid, metadata_invalid, target, job.pixel_to_micron,
                job.frame_selection.value, core.source_base_name(job.input_path))
        else:
            self.valid_count, self.invalid_count = core.export_metrics_to_csv(
                metadata_valid, metadata_invalid, target, job.pixel_to_micron, job.frame_selection.value)
        self.completed_units += 1
        self.progress(ExportPhase.METRICS, str(target),
                      f"Exported {self.valid_count + self.invalid_count} frames "
                      f"(Valid: {self.valid_count}, Invalid: {self.invalid_count})")

    def export_frame_images(self, h5_file: Any, dataset_path: str, metadata: Any,
                            prefix: str, phase: ExportPhase, out_dir: Path) -> None:
        if dataset_path not in h5_file:
            self.warn(f"{dataset_path} dataset not found")
            return
        dataset = h5_file[dataset_path]
        count = min(dataset_length(dataset), dataset_length(metadata))
        self.progress(phase, message=f"Exporting {prefix} frame images ({count})...")
        for i in range(count):
            _check_cancel(self.cancel_event)
            frame_index = int(metadata[i]["index"])
            destination = out_dir / f"{prefix}_frame_{frame_index:06d}.tiff"
            # One frame resident at a time; released before the next read.
            image = dataset[i]
            try:
                ok = self.image_writer(image, destination, i)
            finally:
                del image
            if ok:
                self.images_exported += 1
            else:
                self.images_skipped += 1
                self.warn(f"skipped image {destination.name} (unsupported shape)")
            self.completed_units += 1
            if (i + 1) % 25 == 0 or i + 1 == count:
                self.progress(phase, str(destination))

    def export_series_images(self, h5_file: Any, metadata: Any, out_dir: Path) -> None:
        job = self.job
        dataset_path = "/valid_frames/series_images"
        if not job.export_series or dataset_path not in h5_file:
            return
        series_ds = h5_file[dataset_path]
        shape = tuple(getattr(series_ds, "shape", ()))
        if len(shape) != 4:
            self.warn(f"series_images has unexpected shape {shape}")
            return
        n_records, series_count = int(shape[0]), int(shape[1])
        start, end = 0, series_count - 1
        if job.series_range is not None:
            start = min(job.series_range[0], series_count - 1)
            end = min(job.series_range[1], series_count - 1)
        records = min(n_records, dataset_length(metadata))
        self.progress(ExportPhase.SERIES_IMAGES,
                      message=f"Exporting series images ({records} records x {end - start + 1})...")
        for i in range(records):
            _check_cancel(self.cancel_event)
            frame_index = int(metadata[i]["index"])
            for s in range(start, end + 1):
                _check_cancel(self.cancel_event)
                destination = out_dir / f"valid_frame_{frame_index:06d}_series_{s:02d}.tiff"
                image = series_ds[i, s]  # one series frame resident at a time
                try:
                    ok = self.image_writer(image, destination, i)
                finally:
                    del image
                if ok:
                    self.series_exported += 1
                else:
                    self.images_skipped += 1
                    self.warn(f"skipped series image {destination.name} (unsupported shape)")
                self.completed_units += 1
            if (i + 1) % 10 == 0 or i + 1 == records:
                self.progress(ExportPhase.SERIES_IMAGES, str(out_dir))

    def _plan_units(self, h5_file: Any, metadata_valid: Any, metadata_invalid: Any) -> None:
        job = self.job
        units = 1 if (job.format.writes_csv or job.format == ExportFormat.JSON) else 0
        if job.format.writes_images:
            if metadata_valid is not None and "/valid_frames/images" in h5_file:
                units += min(dataset_length(h5_file["/valid_frames/images"]), dataset_length(metadata_valid))
            if metadata_valid is not None and job.export_series and "/valid_frames/series_images" in h5_file:
                shape = tuple(getattr(h5_file["/valid_frames/series_images"], "shape", ()))
                if len(shape) == 4:
                    per_record = int(shape[1])
                    if job.series_range is not None:
                        per_record = min(job.series_range[1], int(shape[1]) - 1) - min(job.series_range[0], int(shape[1]) - 1) + 1
                    units += min(int(shape[0]), dataset_length(metadata_valid)) * max(per_record, 0)
            if metadata_invalid is not None and "/invalid_frames/images" in h5_file:
                units += min(dataset_length(h5_file["/invalid_frames/images"]), dataset_length(metadata_invalid))
        self.total_units = units

    # -- main --------------------------------------------------------------
    def execute(self) -> ExportResult:
        job = self.job
        started = time.monotonic()
        state = ExportState.FAILED
        error = ""
        try:
            self.progress(ExportPhase.VALIDATING, message="Validating job...")
            if not job.input_path.exists():
                raise ExportFailed(f"Input file does not exist: {job.input_path}")
            if not job.input_path.is_file():
                raise ExportFailed(f"Input path is not a file: {job.input_path}")
            ok, why = core.ensure_output_root(job.output_root)
            if not ok:
                raise ExportFailed(why)
            if not core.HAS_HDF5_DEPS:
                raise ExportFailed("Required dependencies not installed. Install with: pip install h5py numpy. "
                                   f"Details: {core.HDF5_IMPORT_ERROR}")
            if job.format.writes_images and not core.HAS_CV2:
                raise ExportFailed("opencv-python (cv2) is required for image export.")
            _check_cancel(self.cancel_event)

            self.final_path, self.partial = self.choose_destination()
            metrics_target = None
            if job.format.writes_images:
                out_dir = self.partial
                if job.format == ExportFormat.ALL:
                    metrics_target = out_dir / "metrics.csv"
            else:
                out_dir = None
                metrics_target = self.partial

            _check_cancel(self.cancel_event)
            self.progress(ExportPhase.METADATA, message="Opening HDF5 file...")
            with core.h5py.File(job.input_path, "r") as h5_file:
                metadata_valid = None
                metadata_invalid = None
                if job.frame_selection.includes_valid:
                    metadata_valid = core.read_hdf5_metadata(h5_file, "/valid_frames/metadata")
                    if metadata_valid is None:
                        self.warn("/valid_frames/metadata dataset not found")
                if job.frame_selection.includes_invalid:
                    metadata_invalid = core.read_hdf5_metadata(h5_file, "/invalid_frames/metadata")
                    if metadata_invalid is None:
                        self.warn("/invalid_frames/metadata dataset not found")
                _check_cancel(self.cancel_event)
                if metadata_valid is None and metadata_invalid is None:
                    raise ExportFailed("No metadata found in HDF5 file")
                self._plan_units(h5_file, metadata_valid, metadata_invalid)

                if metrics_target is not None:
                    self.export_metrics(metadata_valid, metadata_invalid, metrics_target)

                if job.format.writes_images:
                    assert out_dir is not None
                    if job.frame_selection.includes_valid and metadata_valid is not None:
                        self.export_frame_images(h5_file, "/valid_frames/images", metadata_valid,
                                                 "valid", ExportPhase.VALID_IMAGES, out_dir)
                        self.export_series_images(h5_file, metadata_valid, out_dir)
                    if job.frame_selection.includes_invalid and metadata_invalid is not None:
                        self.export_frame_images(h5_file, "/invalid_frames/images", metadata_invalid,
                                                 "invalid", ExportPhase.INVALID_IMAGES, out_dir)
                    self.progress(ExportPhase.INVALID_IMAGES,
                                  message=f"Total images exported: {self.images_exported + self.series_exported}")

                info = core.read_experiment_info(h5_file)
                if info:
                    self.experiment_info = {str(k): _jsonable(v) for k, v in info.items()}

            _check_cancel(self.cancel_event)
            self.progress(ExportPhase.COMMITTING, str(self.final_path), "Publishing output...")
            self.final_path = self.commit()
            self.partial = None
            state = ExportState.COMPLETED
        except ExportCancelled:
            state = ExportState.CANCELLED
            error = "Export cancelled by user"
        except ExportFailed as exc:
            state = ExportState.FAILED
            error = str(exc)
        except (IOError, OSError) as exc:
            state = ExportState.FAILED
            error = f"Failed to open or write: {exc}"
        except Exception as exc:  # noqa: BLE001 - structured failure for callers
            state = ExportState.FAILED
            error = f"Unexpected error: {exc!r}"

        retained: Optional[Path] = None
        if state != ExportState.COMPLETED:
            self.progress(ExportPhase.CLEANUP, message="Discarding partial output...")
            retained = _discard_partial(self.partial, job, state, error, self.counts())
        return ExportResult(
            job_id=job.job_id,
            state=state,
            final_path=self.final_path if state == ExportState.COMPLETED else None,
            valid_count=self.valid_count,
            invalid_count=self.invalid_count,
            images_exported=self.images_exported,
            images_skipped=self.images_skipped,
            series_exported=self.series_exported,
            duration_s=time.monotonic() - started,
            warnings=tuple(self.warnings),
            error=error,
            partial_path=retained,
            experiment_info=self.experiment_info,
        )


def _jsonable(value: Any) -> Any:
    try:
        if hasattr(value, "tolist"):
            return value.tolist()
        if isinstance(value, bytes):
            return value.decode("utf-8", "replace")
    except Exception:  # noqa: BLE001
        pass
    return value if isinstance(value, (int, float, str, bool, list, dict)) or value is None else str(value)


def run_export_job(job: ExportJob, *, cancel_event: Optional[threading.Event] = None,
                   on_progress: Optional[ProgressCallback] = None,
                   image_writer: Optional[ImageWriter] = None) -> ExportResult:
    """Execute one immutable export job and return its terminal result.

    Never raises for job failures (they become ``ExportState.FAILED``);
    ``image_writer`` is an injection seam for tests/fault injection.
    """
    writer = image_writer if image_writer is not None else write_frame_image
    return _Run(job, cancel_event, on_progress, writer).execute()


def print_progress(progress: ExportProgress, stream=None) -> None:
    """Default CLI progress sink."""
    stream = stream or sys.stdout
    if progress.message:
        print(progress.message, file=sys.stderr if progress.message.startswith("WARNING") else stream)
