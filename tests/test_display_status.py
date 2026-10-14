#!/usr/bin/env python3
"""Offline contract tests for the read-only YOFO status display companion."""
from __future__ import annotations

import contextlib
import io
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).parents[1]))
from tools.display_status.yofo_status_display import (  # noqa: E402
    DisplayPainter,
    StudioStatusClient,
    classify_status,
    run,
    load_display_tool,
    StatusProtocolError,
    _status_age,
)


def good_status() -> dict:
    return {
        "available": True,
        "mode": {"name": "run"},
        "core": {"build_match": "match", "profile_match": "match"},
        "led": {"guard_fault": False},
        "link": {"ingress_errors_warn": False, "bad_frames_warn": False, "dropped_warn": False,
                 "rx_heal": {"gave_up": False}},
    }


class FakeWebSocket:
    def __init__(self, replies):
        self.replies = list(replies)
        self.sent = []
        self.closed = False

    def send(self, payload):
        self.sent.append(json.loads(payload))

    def recv(self, timeout):
        if not self.replies:
            raise TimeoutError
        return self.replies.pop(0)

    def close(self):
        self.closed = True


class FakeLock:
    def __enter__(self):
        return self

    def __exit__(self, *_):
        return False


class FakeLink:
    def __init__(self, **_):
        self.closed = False

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.closed = True

    def uart_configure(self, *_):
        pass


class FakeDisplay:
    def __init__(self, link, _timeout):
        self.pages = []
        self.brightness = 42
        self.cleaned = False
        self.fail_page_read = False

    def configure(self):
        pass

    def read_register(self, address):
        if address == 0x7000 and self.fail_page_read:
            raise RuntimeError("simulated page readback failure")
        return (self.pages[-1] if address == 0x7000 and self.pages else 0).to_bytes(2, "big") if address == 0x7000 else self.brightness.to_bytes(2, "big")

    def write_register(self, address, value):
        if address == 0x7000:
            self.pages.append(value)
        else:
            self.brightness = value

    def cleanup(self):
        self.cleaned = True


class FakeDisplayModule:
    TARGET_PAGES = (6, 7, 8)
    PAGE_REGISTER = 0x7000
    BACKLIGHT_REGISTER = 0x7001
    LiveClient = FakeLink
    DisplayLink = FakeDisplay

    @staticmethod
    def exclusive_lock(_path):
        return FakeLock()

    @staticmethod
    def inspect_layout(_display, strict=True, require_patchable=True):
        pages = bytearray(9 * 12)
        widgets = bytearray(9 * 12)
        for page in FakeDisplayModule.TARGET_PAGES:
            pages[page * 12 + 4:page * 12 + 8] = (1024).to_bytes(2, "little") + (600).to_bytes(2, "little")
            widgets[page * 12 + 4:page * 12 + 6] = b"\0\0"
            widgets[page * 12 + 10:page * 12 + 12] = b"\0\0"
        pages, widgets = FakeDisplayModule.patch_page_records(bytes(pages), bytes(widgets), 1)
        return {"pages": pages.hex(), "widgets": widgets.hex(), "tft_bitcolor": 1}

    @staticmethod
    def patch_page_records(pages, widgets, mode):
        pages = bytearray(pages)
        for page, color in zip(FakeDisplayModule.TARGET_PAGES, (0xFF0000, 0x00FF00, 0x0000FF)):
            pages[page * 12 + 8:page * 12 + 11] = color.to_bytes(3, "little")
            pages[page * 12 + 11] = 0x10
        return bytes(pages), widgets


class DisplayStatusTests(unittest.TestCase):
    def test_dynamic_adapter_with_dataclass_loads(self):
        import tempfile
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "adapter.py"
            path.write_text("from __future__ import annotations\nfrom dataclasses import dataclass\n@dataclass\nclass Frame:\n    body: bytes\n", encoding="utf-8")
            module = load_display_tool(path)
            self.assertEqual(module.Frame(b"ok").body, b"ok")

    def test_mismatched_replies_ignored_and_error_not_green(self):
        ws = FakeWebSocket(["not json", json.dumps({"request_id": 7, "ok": good_status()}),
                            json.dumps({"request_id": 1, "error": "unavailable"})])
        with self.assertRaises(StatusProtocolError):
            StudioStatusClient(ws).fetch(timeout_s=1)
        self.assertEqual(len(ws.sent), 1)

    def test_malformed_nested_status_is_blue(self):
        self.assertEqual(classify_status({"available": True, "led": "broken"}, age_s=0, stale_after_s=3).page, 8)
        self.assertEqual(classify_status({**good_status(), "led": {"guard_fault": "false"}}, age_s=0, stale_after_s=3).page, 8)
        self.assertEqual(classify_status({**good_status(), "link": {"rx_heal": "broken"}}, age_s=0, stale_after_s=3).page, 8)
        self.assertEqual(classify_status({**good_status(), "core": {"build_match": "unknown", "profile_match": "match"}}, age_s=0, stale_after_s=3).page, 8)
        # `mode.connected` is not a current InstrumentStatus field and must
        # not be invented as a second disconnected-state contract.
        self.assertEqual(classify_status({**good_status(), "mode": {"name": "run", "connected": False}}, age_s=0, stale_after_s=3).page, 7)
        for timestamp in (float("nan"), float("inf"), 11.0, "bad"):
            age = _status_age(10.0, {"timestamp": timestamp})
            self.assertEqual(classify_status(good_status(), age_s=age, stale_after_s=3).page, 8)

    def test_classifier_contract(self):
        self.assertEqual(classify_status(None, age_s=None, stale_after_s=3).page, 8)
        self.assertEqual(classify_status({"available": "yes"}, age_s=0, stale_after_s=3).page, 8)
        self.assertEqual(classify_status({"available": False}, age_s=0, stale_after_s=3).page, 8)
        self.assertEqual(classify_status({"available": True, "stale": True}, age_s=0, stale_after_s=3).page, 8)
        self.assertEqual(classify_status(good_status(), age_s=4, stale_after_s=3).page, 8)
        self.assertEqual(classify_status({**good_status(), "error": "core offline"}, age_s=0, stale_after_s=3).page, 6)
        self.assertEqual(classify_status({**good_status(), "led": {"guard_fault": True}}, age_s=0, stale_after_s=3).page, 6)
        self.assertEqual(classify_status({**good_status(), "core": {"build_match": "mismatch", "profile_match": "match"}}, age_s=0, stale_after_s=3).page, 6)
        self.assertEqual(classify_status({**good_status(), "link": {"rx_heal": {"gave_up": True}}}, age_s=0, stale_after_s=3).page, 6)
        green = classify_status(good_status(), age_s=0, stale_after_s=3)
        self.assertEqual((green.page, green.label), (7, "report received"))

    def test_status_client_allowlist_and_no_acquisition_commands(self):
        ws = FakeWebSocket([json.dumps({"request_id": 1, "ok": good_status()})])
        result = StudioStatusClient(ws).fetch(timeout_s=1)
        self.assertTrue(result["available"])
        self.assertEqual([item["cmd"] for item in ws.sent], ["fetch_instrument_status"])

    def test_display_repaints_only_on_change_and_cleans_up(self):
        args = type("Args", (), {"display_lock": "/tmp/test-display.lock", "fpga": "fpga", "host": "host", "display_timeout": 1.0})()
        painter = DisplayPainter(FakeDisplayModule, args)
        painter.start()
        display = painter.display
        before = len(display.pages)
        painter.update(7)
        painter.update(7)
        self.assertEqual(len(display.pages), before + 1)
        painter.close()
        self.assertTrue(display.cleaned)

    def test_cleanup_restores_page_and_brightness_independently(self):
        args = type("Args", (), {"display_lock": "/tmp/test-display.lock", "fpga": "fpga", "host": "host", "display_timeout": 1.0})()
        painter = DisplayPainter(FakeDisplayModule, args)
        painter.start()
        display = painter.display
        display.fail_page_read = True
        with self.assertRaises(StatusProtocolError):
            painter.close()
        self.assertEqual(display.pages[-1], 0)
        self.assertEqual(display.brightness, 42)
        self.assertTrue(display.cleaned)

    def test_run_prints_json_and_reconnects_as_blue_on_transport_failure(self):
        sockets = []

        def factory(_url, _timeout):
            ws = FakeWebSocket([json.dumps({"request_id": 1, "ok": good_status()})])
            sockets.append(ws)
            return ws

        args = type("Args", (), {
            "duration": 0, "poll": 1.0, "stale_after": 3.0, "request_timeout": 1.0,
            "display_tool": None, "display_lock": "/tmp/test-display.lock", "display_timeout": 1.0,
            "fpga": "fpga", "host": "host", "studio_url": "http://studio",
        })()
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(run(args, ws_factory=factory), 0)
        report = json.loads(output.getvalue())
        self.assertEqual(report["page"], 7)
        self.assertEqual(sockets[0].sent[0]["cmd"], "fetch_instrument_status")

    def test_unhashable_identity_is_blue(self):
        for field, key in (("mode", "name"), ("core", "build_match"), ("core", "profile_match")):
            for value in ([], {}):
                status = good_status()
                status[field][key] = value
                self.assertEqual(classify_status(status, age_s=0, stale_after_s=3).page, 8)

    def test_existing_age_shortens_freshness_lease(self):
        from tools.display_status.yofo_status_display import build_parser
        now = [0.0]
        sleeps = []
        status = good_status()
        status["age_ms"] = 2900
        ws = FakeWebSocket([json.dumps({"request_id": 1, "ok": status})])
        def sleep(seconds):
            sleeps.append(seconds)
            now[0] += seconds
        args = build_parser().parse_args(["--duration", "1", "--poll", "3", "--request-timeout", "3"])
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            run(args, ws_factory=lambda *_: ws, clock=lambda: now[0], sleep_fn=sleep)
        self.assertAlmostEqual(sleeps[0], 0.1)
        reports = [json.loads(line) for line in output.getvalue().splitlines()]
        self.assertEqual(reports[1]["page"], 8)

    def test_monotonic_freshness_expiry_paints_blue(self):
        clock_value = [0.0]
        ws = FakeWebSocket([json.dumps({"request_id": 1, "ok": good_status()})])

        def factory(_url, _timeout):
            return ws

        def clock():
            return clock_value[0]

        def sleep(seconds):
            clock_value[0] += seconds

        args = type("Args", (), {
            "duration": 6, "poll": 3.0, "stale_after": 3.0, "request_timeout": 3.0,
            "display_tool": None, "display_lock": "/tmp/test-display.lock", "display_timeout": 1.0,
            "fpga": "fpga", "host": "host", "studio_url": "http://studio",
        })()
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(run(args, ws_factory=factory, clock=clock, sleep_fn=sleep), 0)
        reports = [json.loads(line) for line in output.getvalue().splitlines()]
        self.assertEqual(reports[0]["page"], 7)
        self.assertEqual(reports[1]["reason"], "stale")
        self.assertEqual(reports[1]["page"], 8)
        self.assertNotEqual(reports[-1]["page"], 7)


if __name__ == "__main__":
    unittest.main()
