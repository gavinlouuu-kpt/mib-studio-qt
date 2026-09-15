"""
Qt adapter for the bounded-memory HDF5 export engine (issue #344).

``ExportWorker`` is a ``QObject`` that owns one immutable ``ExportJob`` and a
shared ``threading.Event`` cancellation token. It is moved to a ``QThread`` by
the GUI; its ``run`` slot takes no arguments (nothing about the job depends on
mutable widget state), emits ``progress``/``result`` and finally the
no-argument ``finished`` signal that drives deterministic teardown
(``thread.quit`` / ``deleteLater``).

The engine itself (``hdf_export_engine``) has no Qt dependency.
"""

from __future__ import annotations

import logging
import threading
from typing import Callable, Optional

from PySide6.QtCore import QObject, Signal, Slot

from hdf_export_engine import (
    ExportJob,
    ExportProgress,
    ExportResult,
    ExportState,
    run_export_job,
)

log = logging.getLogger("hdf5_export")

Runner = Callable[..., ExportResult]


class ExportWorker(QObject):
    """Runs one ExportJob on the thread it has been moved to."""

    progress = Signal(object)  # ExportProgress
    result = Signal(object)    # ExportResult (exactly once)
    finished = Signal()        # terminal, no arguments (drives teardown)

    def __init__(self, job: ExportJob, cancel_event: threading.Event,
                 runner: Optional[Runner] = None, parent: Optional[QObject] = None) -> None:
        super().__init__(parent)
        self._job = job
        self._cancel_event = cancel_event
        self._runner: Runner = runner or run_export_job
        self._ran = False

    @property
    def job(self) -> ExportJob:
        return self._job

    def cancel(self) -> None:
        """Thread-safe cancellation request (idempotent)."""
        self._cancel_event.set()

    @Slot()
    def run(self) -> None:
        if self._ran:  # a worker runs exactly once
            return
        self._ran = True
        job = self._job
        log.info("export %s: started (%s, %s, %s -> %s)", job.job_id, job.format.value,
                 job.frame_selection.value, job.input_path, job.output_root)
        try:
            result = self._runner(job, cancel_event=self._cancel_event, on_progress=self.progress.emit)
        except Exception as exc:  # noqa: BLE001 - never let a worker die without a result
            log.exception("export %s: worker raised", job.job_id)
            result = ExportResult(job_id=job.job_id, state=ExportState.FAILED, final_path=None,
                                  error=f"Worker exception: {exc!r}")
        if result.state == ExportState.COMPLETED:
            log.info("export %s: completed in %.2fs -> %s", job.job_id, result.duration_s, result.final_path)
        elif result.state == ExportState.CANCELLED:
            log.info("export %s: cancelled after %.2fs", job.job_id, result.duration_s)
        else:
            log.error("export %s: failed after %.2fs: %s", job.job_id, result.duration_s, result.error)
        self.result.emit(result)
        self.finished.emit()


__all__ = ["ExportWorker", "ExportJob", "ExportProgress", "ExportResult", "ExportState"]
