#!/usr/bin/env python3
"""
HDF5 Export GUI Application

A PySide6-based GUI application for exporting metrics and images from MIB
Studio HDF5 files.

Lifecycle contract (issue #344): one active export per window; each job is an
immutable ``ExportJob`` run by an ``ExportWorker`` on its own ``QThread`` with
the deterministic teardown chain (``worker.finished -> thread.quit``,
``worker.finished -> worker.deleteLater``, ``thread.finished ->
thread.deleteLater``); the GUI thread never blocks in ``QThread.wait()``;
references are cleared only from the thread-finished handler; closing the
window during an export requests cancellation and completes the close after
the thread has finished. ``QThread.terminate()`` is never used.
"""

from __future__ import annotations

import logging
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Optional

try:
    from PySide6.QtWidgets import (
        QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
        QPushButton, QLabel, QLineEdit, QComboBox, QDoubleSpinBox,
        QProgressBar, QTextEdit, QFileDialog, QMessageBox, QGroupBox
    )
    from PySide6.QtCore import Qt, QThread, QTimer
except ImportError as e:
    print("ERROR: PySide6 is required for the GUI application.", file=sys.stderr)
    print("Install with: pip install PySide6", file=sys.stderr)
    print(f"Details: {e}", file=sys.stderr)
    sys.exit(1)

from export_worker import ExportWorker
from hdf_export_engine import ExportJob, ExportPhase, ExportProgress, ExportResult, ExportState

log = logging.getLogger("hdf5_export")

_PHASE_LABEL = {
    ExportPhase.VALIDATING: "Validating",
    ExportPhase.METADATA: "Reading metadata",
    ExportPhase.METRICS: "Exporting metrics",
    ExportPhase.VALID_IMAGES: "Exporting valid images",
    ExportPhase.INVALID_IMAGES: "Exporting invalid images",
    ExportPhase.SERIES_IMAGES: "Exporting series images",
    ExportPhase.COMMITTING: "Publishing output",
    ExportPhase.CLEANUP: "Cleaning up",
}


@dataclass
class ActiveExport:
    """Strong references for the one in-flight job (held until thread.finished)."""
    job: ExportJob
    thread: QThread
    worker: ExportWorker
    cancel_event: threading.Event
    started_at: float
    result: Optional[ExportResult] = None
    cancel_requested: bool = False


class ExportWindow(QMainWindow):
    """Main window for the HDF5 export application."""

    def __init__(self, runner: Optional[Callable] = None, interactive: bool = True):
        super().__init__()
        self._runner = runner          # engine runner injection (tests)
        self._interactive = interactive  # False: no modal message boxes
        self.active: Optional[ActiveExport] = None
        self.close_pending = False
        self.last_result: Optional[ExportResult] = None
        # Counters for lifecycle tests/soak evidence.
        self.exports_started = 0
        self.exports_finished = 0
        self.workers_destroyed = 0
        self.threads_destroyed = 0
        self.init_ui()

    def init_ui(self):
        """Initialize the user interface."""
        self.setWindowTitle("HDF5 Export Tool")
        self.setMinimumWidth(600)
        self.setMinimumHeight(500)

        central_widget = QWidget()
        self.setCentralWidget(central_widget)
        layout = QVBoxLayout(central_widget)
        layout.setSpacing(10)
        layout.setContentsMargins(15, 15, 15, 15)

        input_group = QGroupBox("Input File")
        input_layout = QVBoxLayout()
        input_file_layout = QHBoxLayout()
        self.input_file_edit = QLineEdit()
        self.input_file_edit.setPlaceholderText("Select HDF5 file...")
        input_file_btn = QPushButton("Browse...")
        input_file_btn.clicked.connect(self.browse_input_file)
        input_file_layout.addWidget(self.input_file_edit)
        input_file_layout.addWidget(input_file_btn)
        input_layout.addLayout(input_file_layout)
        input_group.setLayout(input_layout)
        layout.addWidget(input_group)

        output_group = QGroupBox("Output Directory")
        output_layout = QVBoxLayout()
        output_dir_layout = QHBoxLayout()
        self.output_dir_edit = QLineEdit()
        self.output_dir_edit.setPlaceholderText("Select output directory...")
        output_dir_btn = QPushButton("Browse...")
        output_dir_btn.clicked.connect(self.browse_output_dir)
        output_dir_layout.addWidget(self.output_dir_edit)
        output_dir_layout.addWidget(output_dir_btn)
        output_layout.addLayout(output_dir_layout)
        output_group.setLayout(output_layout)
        layout.addWidget(output_group)

        options_group = QGroupBox("Export Options")
        options_layout = QVBoxLayout()
        format_layout = QHBoxLayout()
        format_layout.addWidget(QLabel("Format:"))
        self.format_combo = QComboBox()
        self.format_combo.addItems(["CSV (metrics only)", "Images only", "All (CSV + Images)"])
        self.format_combo.setCurrentIndex(0)
        format_layout.addWidget(self.format_combo)
        format_layout.addStretch()
        options_layout.addLayout(format_layout)

        frame_type_layout = QHBoxLayout()
        frame_type_layout.addWidget(QLabel("Frame Type:"))
        self.frame_type_combo = QComboBox()
        self.frame_type_combo.addItems(["Both", "Valid only", "Invalid only"])
        self.frame_type_combo.setCurrentIndex(0)
        frame_type_layout.addWidget(self.frame_type_combo)
        frame_type_layout.addStretch()
        options_layout.addLayout(frame_type_layout)

        pixel_layout = QHBoxLayout()
        pixel_layout.addWidget(QLabel("Pixel to Micron:"))
        self.pixel_to_micron_spin = QDoubleSpinBox()
        self.pixel_to_micron_spin.setRange(0.0001, 1000.0)
        self.pixel_to_micron_spin.setDecimals(4)
        self.pixel_to_micron_spin.setSingleStep(0.0001)
        self.pixel_to_micron_spin.setValue(0.4886)
        pixel_layout.addWidget(self.pixel_to_micron_spin)
        pixel_layout.addStretch()
        options_layout.addLayout(pixel_layout)
        options_group.setLayout(options_layout)
        layout.addWidget(options_group)

        self.phase_label = QLabel("Idle")
        layout.addWidget(self.phase_label)
        self.progress_bar = QProgressBar()
        self.progress_bar.setRange(0, 100)
        self.progress_bar.setValue(0)
        layout.addWidget(self.progress_bar)

        self.status_text = QTextEdit()
        self.status_text.setReadOnly(True)
        self.status_text.setMaximumHeight(150)
        layout.addWidget(QLabel("Status:"))
        layout.addWidget(self.status_text)

        button_layout = QHBoxLayout()
        button_layout.addStretch()
        self.export_btn = QPushButton("Export")
        self.export_btn.setMinimumWidth(120)
        self.export_btn.clicked.connect(self.start_export)
        button_layout.addWidget(self.export_btn)
        self.cancel_btn = QPushButton("Cancel")
        self.cancel_btn.setMinimumWidth(120)
        self.cancel_btn.setEnabled(False)
        self.cancel_btn.clicked.connect(self.cancel_export)
        button_layout.addWidget(self.cancel_btn)
        button_layout.addStretch()
        layout.addLayout(button_layout)
        layout.addStretch()

    # ------------------------------------------------------------------
    # Input helpers
    # ------------------------------------------------------------------
    def browse_input_file(self):
        file_path, _ = QFileDialog.getOpenFileName(self, "Select HDF5 File", "",
                                                   "HDF5 Files (*.h5 *.hdf5);;All Files (*)")
        if file_path:
            self.input_file_edit.setText(file_path)

    def browse_output_dir(self):
        dir_path = QFileDialog.getExistingDirectory(self, "Select Output Directory", "")
        if dir_path:
            self.output_dir_edit.setText(dir_path)

    def get_format_type(self) -> str:
        index = self.format_combo.currentIndex()
        return "csv" if index == 0 else "images" if index == 1 else "all"

    def get_frame_type(self) -> str:
        index = self.frame_type_combo.currentIndex()
        return "both" if index == 0 else "valid" if index == 1 else "invalid"

    def validate_inputs(self) -> tuple[bool, str]:
        if not self.input_file_edit.text().strip():
            return False, "Please select an input HDF5 file."
        if not self.output_dir_edit.text().strip():
            return False, "Please select an output directory."
        return True, ""

    def build_job(self) -> ExportJob:
        """Freeze the current widget state into an immutable job."""
        return ExportJob(
            input_path=Path(self.input_file_edit.text().strip()),
            output_root=Path(self.output_dir_edit.text().strip()),
            format=self.get_format_type(),
            frame_selection=self.get_frame_type(),
            pixel_to_micron=self.pixel_to_micron_spin.value(),
        )

    def _notify(self, kind: str, title: str, message: str) -> None:
        if not self._interactive:
            return
        if kind == "critical":
            QMessageBox.critical(self, title, message)
        elif kind == "warning":
            QMessageBox.warning(self, title, message)
        else:
            QMessageBox.information(self, title, message)

    # ------------------------------------------------------------------
    # Export lifecycle
    # ------------------------------------------------------------------
    @property
    def export_active(self) -> bool:
        return self.active is not None

    def start_export(self) -> bool:
        """Start one export; returns False when refused (invalid input or busy)."""
        if self.active is not None:
            self.update_status(f"An export is already running (job {self.active.job.job_id}).")
            return False
        valid, error_msg = self.validate_inputs()
        if not valid:
            self._notify("warning", "Validation Error", error_msg)
            return False
        try:
            job = self.build_job()
        except ValueError as exc:
            self._notify("warning", "Validation Error", str(exc))
            return False
        return self.start_job(job)

    def start_job(self, job: ExportJob) -> bool:
        """Run an already-built immutable job (single-flight)."""
        if self.active is not None:
            return False
        cancel_event = threading.Event()
        thread = QThread(self)
        thread.setObjectName(f"export-{job.job_id}")
        worker = ExportWorker(job, cancel_event, runner=self._runner)
        worker.moveToThread(thread)

        # Deterministic completion chain.
        thread.started.connect(worker.run)
        worker.finished.connect(thread.quit)
        worker.finished.connect(worker.deleteLater)
        thread.finished.connect(thread.deleteLater)
        worker.progress.connect(self.on_export_progress)
        worker.result.connect(self.on_export_result)
        thread.finished.connect(self.on_export_thread_finished)
        worker.destroyed.connect(self._on_worker_destroyed)
        thread.destroyed.connect(self._on_thread_destroyed)

        self.active = ActiveExport(job=job, thread=thread, worker=worker,
                                   cancel_event=cancel_event, started_at=time.monotonic())
        self.exports_started += 1
        self.export_btn.setEnabled(False)
        self.cancel_btn.setEnabled(True)
        self.status_text.clear()
        self.progress_bar.setValue(0)
        self.phase_label.setText("Starting...")
        self.update_status(f"Job {job.job_id}: {job.format.value} / {job.frame_selection.value}")
        log.info("export %s: thread start", job.job_id)
        thread.start()
        return True

    def cancel_export(self):
        """Request cancellation (idempotent; the button stays disabled)."""
        active = self.active
        if active is None:
            return
        if not active.cancel_requested:
            active.cancel_requested = True
            active.cancel_event.set()
            log.info("export %s: cancellation requested", active.job.job_id)
            self.update_status("Cancelling export...")
        self.cancel_btn.setEnabled(False)

    def on_export_progress(self, progress: ExportProgress):
        if self.active is None or progress.job_id != self.active.job.job_id:
            return  # stale event from a finished job
        self.phase_label.setText(_PHASE_LABEL.get(progress.phase, progress.phase.value))
        if progress.total > 0:
            value = int(min(100, max(0, round(100 * progress.completed / progress.total))))
            if value > self.progress_bar.value():  # monotonic
                self.progress_bar.setValue(value)
        if progress.message:
            self.update_status(progress.message)

    def update_status(self, message: str):
        self.status_text.append(message)
        scrollbar = self.status_text.verticalScrollBar()
        scrollbar.setValue(scrollbar.maximum())

    def on_export_result(self, result: ExportResult):
        """Terminal result (exactly once per job). Controls stay disabled
        until the thread has actually finished."""
        if self.active is None or result.job_id != self.active.job.job_id:
            log.warning("export %s: result for a job that is not active", result.job_id)
            return
        self.active.result = result
        self.last_result = result
        if result.state == ExportState.COMPLETED:
            self.progress_bar.setValue(100)
            self.phase_label.setText("Complete")
            summary = f"Export complete. Output: {result.final_path}"
            if result.experiment_info:
                summary += "\nExperiment Info:\n" + "\n".join(
                    f"  {k}: {v}" for k, v in sorted(result.experiment_info.items()))
            self.update_status(summary)
        elif result.state == ExportState.CANCELLED:
            self.progress_bar.setValue(0)
            self.phase_label.setText("Cancelled")
            self.update_status("Export cancelled; partial output discarded.")
        else:
            self.progress_bar.setValue(0)
            self.phase_label.setText("Failed")
            self.update_status(f"ERROR: {result.error}")
            if result.partial_path is not None:
                self.update_status(f"Partial output retained at: {result.partial_path}")

    def on_export_thread_finished(self):
        """The only place references are cleared and controls re-enabled."""
        active = self.active
        if active is None:
            return
        result = active.result
        self.active = None
        self.exports_finished += 1
        log.info("export %s: thread finished (%.2fs total)", active.job.job_id,
                 time.monotonic() - active.started_at)
        self.export_btn.setEnabled(True)
        self.cancel_btn.setEnabled(False)
        if result is None:
            self.phase_label.setText("Failed")
            self.update_status("ERROR: the export ended without a result.")
        if self.close_pending:
            # Complete the deferred close on the event loop, after teardown.
            QTimer.singleShot(0, self.close)
            return
        if result is not None and not self.close_pending:
            if result.state == ExportState.COMPLETED:
                self._notify("information", "Export Complete", f"Export complete. Output: {result.final_path}")
            elif result.state == ExportState.FAILED:
                self._notify("critical", "Export Failed", result.error)

    def _on_worker_destroyed(self, *_):
        self.workers_destroyed += 1

    def _on_thread_destroyed(self, *_):
        self.threads_destroyed += 1

    # ------------------------------------------------------------------
    # Close policy: never destroy a running QThread
    # ------------------------------------------------------------------
    def closeEvent(self, event):
        if self.active is None:
            self.close_pending = False
            event.accept()
            return
        if not self.close_pending:
            log.info("export %s: close requested while active; cancelling and deferring close",
                     self.active.job.job_id)
            self.close_pending = True
            self.cancel_export()
            self.update_status("Closing after the running export stops...")
        event.ignore()


def main():
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    app = QApplication(sys.argv)
    app.setApplicationName("HDF5 Export Tool")
    window = ExportWindow()
    window.show()
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
