#!/usr/bin/env python3
"""PySide worker/QThread lifecycle tests for the HDF5 Export Tool (issue #344).

Runs the real ``ExportWindow`` + ``ExportWorker`` + engine offscreen:

* repeated exports through the window in one process; starts, finishes and
  worker/thread destructions reconcile; weak references clear;
* a second start while one is active is refused;
* cancellation in every engine phase (injected slow runner);
* close-during-export is deferred until the thread has finished;
* a worker exception becomes a structured failed result and still tears
  down;
* any ``QThread: Destroyed while thread is still running`` (or other Qt
  thread lifecycle warning) fails the test.

Skips cleanly when PySide6/h5py/OpenCV are unavailable. A watchdog exits
with code 99 when the event loop never settles.
"""

from __future__ import annotations

import gc
import os
import sys
import tempfile
import threading
import time
import unittest
import weakref
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))

try:
    from PySide6.QtCore import QCoreApplication, QDeadlineTimer, QEventLoop, QtMsgType, qInstallMessageHandler
    from PySide6.QtWidgets import QApplication

    import export_test_fixture as fixture
    from hdf_export_engine import (
        ExportJob,
        ExportPhase,
        ExportProgress,
        ExportResult,
        ExportState,
        ExportCancelled,
        run_export_job,
    )
    from hdf5_export_app import ExportWindow

    HAS_DEPS = True
except Exception as exc:  # noqa: BLE001
    HAS_DEPS = False
    IMPORT_ERROR = exc

WATCHDOG_SECONDS = 180


def _watchdog():
    time.sleep(WATCHDOG_SECONDS)
    sys.stderr.write("lifecycle test watchdog fired: event loop never settled\n")
    sys.stderr.flush()
    os._exit(99)


_QT_WARNINGS: list[str] = []
_LIFECYCLE_MARKERS = ("Destroyed while thread is still running", "QThread", "QObject::~QObject")


def _message_handler(mode, context, message):
    if mode in (QtMsgType.QtWarningMsg, QtMsgType.QtCriticalMsg, QtMsgType.QtFatalMsg):
        _QT_WARNINGS.append(str(message))
        sys.stderr.write(f"[qt] {message}\n")


def _app() -> QApplication:
    app = QApplication.instance()
    if app is None:
        app = QApplication(sys.argv[:1])
    return app


def process_until(predicate, timeout_s: float = 30.0) -> bool:
    deadline = QDeadlineTimer(int(timeout_s * 1000))
    while not deadline.hasExpired():
        QCoreApplication.processEvents(QEventLoop.AllEvents, 20)
        QCoreApplication.sendPostedEvents(None, 0)  # DeferredDelete
        if predicate():
            return True
        time.sleep(0.002)
    return predicate()


def drain(rounds: int = 5) -> None:
    for _ in range(rounds):
        QCoreApplication.sendPostedEvents(None, 0)
        QCoreApplication.processEvents(QEventLoop.AllEvents, 10)
        gc.collect()


def slow_runner(phase_delays: dict[ExportPhase, float]):
    """Engine stand-in that walks every phase slowly and honours cancellation."""

    def runner(job: ExportJob, *, cancel_event: threading.Event, on_progress, image_writer=None) -> ExportResult:
        started = time.monotonic()
        phases = [ExportPhase.VALIDATING, ExportPhase.METADATA, ExportPhase.METRICS,
                  ExportPhase.VALID_IMAGES, ExportPhase.SERIES_IMAGES, ExportPhase.INVALID_IMAGES,
                  ExportPhase.COMMITTING]
        try:
            for n, phase in enumerate(phases):
                delay = phase_delays.get(phase, 0.0)
                end = time.monotonic() + delay
                while True:
                    if cancel_event.is_set():
                        raise ExportCancelled()
                    on_progress(ExportProgress(job.job_id, phase, n, len(phases), "", ""))
                    if time.monotonic() >= end:
                        break
                    time.sleep(0.005)
            return ExportResult(job.job_id, ExportState.COMPLETED, job.output_root / "fake",
                                duration_s=time.monotonic() - started)
        except ExportCancelled:
            on_progress(ExportProgress(job.job_id, ExportPhase.CLEANUP, 0, 1, "", ""))
            return ExportResult(job.job_id, ExportState.CANCELLED, None, error="Export cancelled by user",
                                duration_s=time.monotonic() - started)

    return runner


def raising_runner(job, *, cancel_event, on_progress, image_writer=None):
    raise RuntimeError("boom inside worker")


@unittest.skipUnless(HAS_DEPS, "PySide6/h5py/opencv not installed")
class ExportWindowLifecycleTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        threading.Thread(target=_watchdog, daemon=True).start()
        qInstallMessageHandler(_message_handler)
        cls.app = _app()
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        cls.source = fixture.write_fixture(cls.root / "fixture.h5", valid_frames=12, invalid_frames=6,
                                           series_count=2, height=16, width=24)
        cls.source_sha = fixture.sha256_of_file(cls.source)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def setUp(self) -> None:
        _QT_WARNINGS.clear()

    def tearDown(self) -> None:
        drain()
        bad = [w for w in _QT_WARNINGS if any(m in w for m in _LIFECYCLE_MARKERS)]
        self.assertEqual(bad, [], f"Qt thread lifecycle warnings: {bad}")
        self.assertEqual(fixture.sha256_of_file(self.source), self.source_sha, "source modified")

    def _window(self, runner=None) -> ExportWindow:
        window = ExportWindow(runner=runner, interactive=False)
        window.input_file_edit.setText(str(self.source))
        out = self.root / f"out-{time.monotonic_ns()}"
        out.mkdir()
        window.output_dir_edit.setText(str(out))
        window.format_combo.setCurrentIndex(2)  # All
        window.show()
        return window

    def _wait_idle(self, window: ExportWindow, timeout_s: float = 60.0) -> None:
        self.assertTrue(process_until(lambda: window.active is None, timeout_s), "export never finished")

    def test_repeated_exports_reconcile_lifecycle_counts(self) -> None:
        window = self._window()
        rounds = 10
        weak_workers = []
        weak_threads = []
        manifests = []
        for _ in range(rounds):
            self.assertTrue(window.start_export())
            self.assertIsNotNone(window.active)
            weak_workers.append(weakref.ref(window.active.worker))
            weak_threads.append(weakref.ref(window.active.thread))
            self.assertFalse(window.export_btn.isEnabled())
            self.assertTrue(window.cancel_btn.isEnabled())
            self._wait_idle(window)
            self.assertEqual(window.last_result.state, ExportState.COMPLETED, window.last_result.error)
            self.assertTrue(window.export_btn.isEnabled())
            self.assertFalse(window.cancel_btn.isEnabled())
            manifests.append(fixture.output_manifest(window.last_result.final_path))
        drain(10)
        self.assertEqual(window.exports_started, rounds)
        self.assertEqual(window.exports_finished, rounds)
        self.assertTrue(process_until(lambda: window.workers_destroyed == rounds, 10), "workers not destroyed")
        self.assertTrue(process_until(lambda: window.threads_destroyed == rounds, 10), "threads not destroyed")
        drain(10)
        self.assertEqual([w() for w in weak_workers], [None] * rounds, "worker wrappers still alive")
        self.assertEqual([t() for t in weak_threads], [None] * rounds, "thread wrappers still alive")
        self.assertTrue(all(m == manifests[0] for m in manifests), "output manifests differ between rounds")
        expected = 1 + fixture.expected_image_count(12, 6, 2)
        self.assertEqual(len(manifests[0]), expected)
        window.close()
        drain()

    def test_second_start_is_refused_while_active(self) -> None:
        window = self._window(runner=slow_runner({ExportPhase.VALID_IMAGES: 0.5}))
        self.assertTrue(window.start_export())
        self.assertFalse(window.start_export(), "second start must be refused")
        self.assertEqual(window.exports_started, 1)
        self._wait_idle(window)
        self.assertEqual(window.exports_finished, 1)
        window.close()

    def test_cancel_in_every_phase(self) -> None:
        for phase in (ExportPhase.METADATA, ExportPhase.VALID_IMAGES, ExportPhase.SERIES_IMAGES,
                      ExportPhase.INVALID_IMAGES, ExportPhase.COMMITTING):
            with self.subTest(phase=phase.value):
                window = self._window(runner=slow_runner({phase: 30.0}))
                self.assertTrue(window.start_export())
                self.assertTrue(process_until(lambda: phase.value in window.phase_label.text().lower()
                                              or window.phase_label.text() != "Starting...", 10))
                started = time.monotonic()
                window.cancel_export()
                self.assertFalse(window.cancel_btn.isEnabled())
                self._wait_idle(window, 10)
                self.assertLess(time.monotonic() - started, 5.0, "cancellation not bounded")
                self.assertEqual(window.last_result.state, ExportState.CANCELLED)
                self.assertTrue(window.export_btn.isEnabled())
                window.close()
                drain()

    def test_cancel_real_engine_during_images(self) -> None:
        window = self._window()
        window.format_combo.setCurrentIndex(1)  # Images
        self.assertTrue(window.start_export())
        # Cancel as soon as the engine reports the first image phase.
        self.assertTrue(process_until(lambda: "images" in window.phase_label.text().lower()
                                      or window.active is None, 30))
        window.cancel_export()
        self._wait_idle(window, 30)
        result = window.last_result
        self.assertIn(result.state, (ExportState.CANCELLED, ExportState.COMPLETED))
        out = Path(window.output_dir_edit.text())
        leftovers = [p.name for p in out.iterdir() if ".partial-" in p.name]
        self.assertEqual(leftovers, [], "partial output leaked after cancel")
        if result.state == ExportState.CANCELLED:
            self.assertFalse((out / "fixture").exists())
        window.close()

    def test_close_during_export_is_deferred_until_thread_finished(self) -> None:
        for phase in (ExportPhase.VALID_IMAGES, ExportPhase.SERIES_IMAGES, ExportPhase.INVALID_IMAGES):
            with self.subTest(phase=phase.value):
                window = self._window(runner=slow_runner({phase: 30.0}))
                self.assertTrue(window.start_export())
                self.assertTrue(process_until(lambda: window.phase_label.text() != "Starting...", 10))
                window.close()
                self.assertTrue(window.close_pending)
                self.assertTrue(window.isVisible(), "window must stay alive while the thread runs")
                self.assertIsNotNone(window.active)
                self.assertTrue(process_until(lambda: not window.isVisible(), 15), "deferred close never happened")
                self.assertIsNone(window.active)
                self.assertEqual(window.last_result.state, ExportState.CANCELLED)
                self.assertTrue(process_until(lambda: window.threads_destroyed == 1, 10))
                drain()

    def test_worker_exception_becomes_failed_result_and_tears_down(self) -> None:
        window = self._window(runner=raising_runner)
        self.assertTrue(window.start_export())
        self._wait_idle(window, 10)
        self.assertEqual(window.last_result.state, ExportState.FAILED)
        self.assertIn("boom", window.last_result.error)
        self.assertTrue(window.export_btn.isEnabled())
        self.assertTrue(process_until(lambda: window.threads_destroyed == 1, 10))
        # The window is usable again.
        window.format_combo.setCurrentIndex(0)
        window._runner = None
        self.assertTrue(window.start_export())
        self._wait_idle(window)
        self.assertEqual(window.last_result.state, ExportState.COMPLETED, window.last_result.error)
        window.close()


if __name__ == "__main__":
    if not HAS_DEPS:
        print(f"SKIP: {IMPORT_ERROR}")
    unittest.main()
