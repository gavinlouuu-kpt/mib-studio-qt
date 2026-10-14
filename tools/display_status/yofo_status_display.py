#!/usr/bin/env python3
"""Show the PZ7035 instrument status on the reviewed ET7689 bench display.

The default mode is a dry run: it reads only ``fetch_instrument_status`` and
prints a bounded JSON status stream.  Passing ``--display-tool`` opts into the
bench adapter, which is loaded from that explicit path and is used only for
page-register repainting.  The adapter's flash/layout checks and UART lock are
part of the safety boundary; this companion never sends a Studio acquisition
or control command.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import importlib.util
import json
from pathlib import Path
import signal
import sys
import time
from typing import Any, Callable, Mapping


BLUE = {"name": "blue", "page": 8, "label": "status unavailable"}
RED = {"name": "red", "page": 6, "label": "status fault"}
GREEN = {"name": "green", "page": 7, "label": "report received"}
ALLOWED_COMMANDS = frozenset({"fetch_instrument_status"})


class StatusProtocolError(RuntimeError):
    """The Studio reply is not a status envelope we can classify safely."""


@dataclass(frozen=True)
class ClassifiedStatus:
    color: str
    page: int
    label: str
    reason: str
    fresh: bool
    available: bool | None

    def as_dict(self, status: Any, sampled_at: float) -> dict[str, Any]:
        return {
            "color": self.color,
            "page": self.page,
            "label": self.label,
            "reason": self.reason,
            "fresh": self.fresh,
            "available": self.available,
            "sampled_at": sampled_at,
            "status": status,
        }


def _is_nonempty(value: Any) -> bool:
    return value not in (None, "", False, [], {})


def _nested(mapping: Mapping[str, Any], *keys: str) -> Any:
    current: Any = mapping
    for key in keys:
        if not isinstance(current, Mapping):
            return None
        current = current.get(key)
    return current


def _reported_warning(status: Mapping[str, Any]) -> str | None:
    """Return the first server-reported warning relevant to display health."""
    for key in ("warning", "warnings"):
        value = status.get(key)
        if _is_nonempty(value):
            return key
    storage_warning = _nested(status, "storage", "warning")
    if _is_nonempty(storage_warning):
        return "storage.warning"
    link = status.get("link")
    if isinstance(link, Mapping):
        for key in ("ingress_errors_warn", "bad_frames_warn", "dropped_warn"):
            if link.get(key) is True:
                return f"link.{key}"
        heal = link.get("rx_heal")
        if isinstance(heal, Mapping) and heal.get("gave_up") is True:
            return "link.rx_heal.gave_up"
    results = status.get("results")
    if isinstance(results, Mapping) and _is_nonempty(results.get("last_error")):
        return "results.last_error"
    return None


def _reported_fault(status: Mapping[str, Any]) -> str | None:
    error = status.get("error")
    if _is_nonempty(error):
        return "error"
    if _nested(status, "led", "guard_fault") is True:
        return "led.guard_fault"
    core = status.get("core")
    if isinstance(core, Mapping):
        for key in ("build_match", "profile_match"):
            if core.get(key) == "mismatch":
                return f"core.{key}"
    # Keep this separate from generic warning handling so the reason is stable.
    heal = _nested(status, "link", "rx_heal")
    if isinstance(heal, Mapping) and heal.get("gave_up") is True:
        return "link.rx_heal.gave_up"
    return _reported_warning(status)


def classify_status(status: Any, *, age_s: float | None, stale_after_s: float) -> ClassifiedStatus:
    """Classify one status sample using the display contract.

    Blue is deliberately the fail-closed state for transport, malformed,
    unavailable, disconnected, and stale samples.  Green means only that a
    fresh valid report arrived; it is not a claim that the instrument is safe,
    running, or available for control.
    """
    if not isinstance(status, Mapping):
        return ClassifiedStatus("blue", 8, BLUE["label"], "malformed", False, None)
    available = status.get("available")
    if not isinstance(available, bool):
        return ClassifiedStatus("blue", 8, BLUE["label"], "malformed.available", False, None)
    if not available:
        return ClassifiedStatus("blue", 8, BLUE["label"], "unavailable", False, False)
    mode = status.get("mode")
    if isinstance(mode, Mapping) and mode.get("connected") is False:
        return ClassifiedStatus("blue", 8, BLUE["label"], "disconnected", False, True)
    if status.get("stale") is True:
        return ClassifiedStatus("blue", 8, BLUE["label"], "stale", False, True)
    if age_s is None or age_s > stale_after_s:
        return ClassifiedStatus("blue", 8, BLUE["label"], "stale", False, True)
    fault = _reported_fault(status)
    if fault:
        return ClassifiedStatus("red", 6, RED["label"], fault, True, True)
    return ClassifiedStatus("green", 7, GREEN["label"], "fresh-valid-report", True, True)


class StudioStatusClient:
    """Small allowlisted synchronous WebSocket client for Studio status."""

    def __init__(self, ws: Any, *, request_id: int = 1) -> None:
        self.ws = ws
        self.request_id = request_id

    def fetch(self, *, timeout_s: float) -> Any:
        command = "fetch_instrument_status"
        if command not in ALLOWED_COMMANDS:
            raise AssertionError("internal command allowlist failure")
        request_id = self.request_id
        self.request_id += 1
        self.ws.send(json.dumps({"request_id": request_id, "cmd": command, "args": None}))
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            remaining = max(0.01, deadline - time.monotonic())
            message = self.ws.recv(timeout=remaining)
            if isinstance(message, bytes):
                continue
            try:
                reply = json.loads(message)
            except (TypeError, ValueError):
                continue
            if not isinstance(reply, Mapping) or reply.get("request_id") != request_id:
                continue
            if "ok" not in reply:
                raise StatusProtocolError(str(reply.get("error", "status request failed")))
            return reply["ok"]
        raise TimeoutError("fetch_instrument_status timed out")


def connect_studio(base_url: str, timeout_s: float) -> Any:
    try:
        from websockets.sync.client import connect
    except ImportError as exc:  # pragma: no cover - depends on host runtime
        raise RuntimeError("websockets is required for live Studio status") from exc
    if not base_url.startswith(("http://", "https://")):
        raise ValueError("Studio URL must use http:// or https://")
    ws_url = "ws" + base_url[4:].rstrip("/") + "/ws"
    return connect(ws_url, open_timeout=timeout_s, close_timeout=timeout_s, max_size=None)


def load_display_tool(path: Path) -> Any:
    if not path.is_file():
        raise FileNotFoundError(path)
    spec = importlib.util.spec_from_file_location("yofo_et7689_display_tool", path)
    if spec is None or spec.loader is None:
        raise ImportError(f"cannot import display tool {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class DisplayPainter:
    """Status-page adapter over the reviewed pz_display_colors module."""

    def __init__(self, module: Any, args: argparse.Namespace) -> None:
        self.module = module
        self.args = args
        self.lock = None
        self.link = None
        self.display = None
        self.original_page: int | None = None
        self.original_brightness: int | None = None
        self.current_page: int | None = None

    def start(self) -> None:
        lock_path = Path(self.args.display_lock)
        self.lock = self.module.exclusive_lock(lock_path)
        self.lock.__enter__()
        try:
            self.link = self.module.LiveClient(
                fpga_ip=self.args.fpga,
                host_ip=self.args.host,
                timeout=min(1.0, self.args.display_timeout),
                retries=1,
            )
            self.link.__enter__()
            self.display = self.module.DisplayLink(self.link, self.args.display_timeout)
            self.display.configure()
            layout = self.module.inspect_layout(self.display, strict=True, require_patchable=True)
            pages = bytes.fromhex(layout["pages"])
            widgets = bytes.fromhex(layout["widgets"])
            expected, _ = self.module.patch_page_records(pages, widgets, layout["tft_bitcolor"])
            for page in self.module.TARGET_PAGES:
                if pages[page * 12:page * 12 + 12] != expected[page * 12:page * 12 + 12]:
                    raise StatusProtocolError(f"color page {page} is not installed")
                record = widgets[page * 12:page * 12 + 12]
                if record[4:6] != b"\0\0" or record[10:12] != b"\0\0":
                    raise StatusProtocolError(f"page {page} still has active overlays")
            self.original_page = int.from_bytes(self.display.read_register(self.module.PAGE_REGISTER), "big")
            self.original_brightness = int.from_bytes(self.display.read_register(self.module.BACKLIGHT_REGISTER), "big")
            self.current_page = self.original_page
            self.repaint(8)
        except BaseException:
            self.close()
            raise

    def repaint(self, page: int) -> None:
        if self.current_page == page:
            return
        self.display.write_register(self.module.PAGE_REGISTER, page)
        actual = int.from_bytes(self.display.read_register(self.module.PAGE_REGISTER), "big")
        if actual != page:
            raise StatusProtocolError(f"display page readback {actual} != {page}")
        self.current_page = page

    def update(self, page: int) -> None:
        if self.display is not None:
            self.repaint(page)

    def close(self) -> None:
        errors: list[str] = []
        if self.display is not None:
            try:
                if self.original_page is not None:
                    self.repaint(self.original_page)
                if self.original_brightness is not None:
                    self.module_value_restore(self.module.BACKLIGHT_REGISTER, self.original_brightness, "brightness")
            except Exception as exc:  # best effort, preserve TX-off cleanup
                errors.append(str(exc))
            try:
                self.display.cleanup()  # Explicitly leaves UART TX disabled.
            except Exception as exc:
                errors.append(f"UART cleanup: {exc}")
        if self.link is not None:
            try:
                self.link.__exit__(None, None, None)
            except Exception as exc:
                errors.append(f"link close: {exc}")
        if self.lock is not None:
            try:
                self.lock.__exit__(None, None, None)
            except Exception as exc:
                errors.append(f"lock close: {exc}")
        self.display = self.link = self.lock = None
        if errors:
            raise StatusProtocolError("display cleanup failed: " + "; ".join(errors))

    def module_value_restore(self, address: int, value: int, label: str) -> None:
        self.display.write_register(address, value)
        actual = int.from_bytes(self.display.read_register(address), "big")
        if actual != value:
            raise StatusProtocolError(f"display {label} readback {actual} != {value}")


def _status_from_reply(reply: Any) -> Any:
    # Studio currently returns the status object directly.  Accepting only the
    # documented object form avoids accidentally treating an error envelope as
    # a healthy report.
    return reply


def _status_age(sampled_at: float, status: Mapping[str, Any]) -> float | None:
    """Extract the few supported freshness representations without guessing units."""
    if status.get("stale") is True:
        return float("inf")
    age_ms = status.get("age_ms")
    if isinstance(age_ms, (int, float)):
        return max(0.0, float(age_ms) / 1000.0)
    for key in ("sampled_at", "timestamp"):
        value = status.get(key)
        if isinstance(value, (int, float)):
            # Unix timestamps are normally seconds; tolerate millisecond
            # timestamps from a bridge DTO without accepting other units.
            seconds = float(value) / 1000.0 if value > 10_000_000_000 else float(value)
            return max(0.0, sampled_at - seconds)
    return 0.0


def run(args: argparse.Namespace, *, ws_factory: Callable[[str, float], Any] = connect_studio) -> int:
    if args.duration < 0 or args.duration > 3600:
        raise ValueError("--duration must be between 0 and 3600 seconds")
    if args.poll <= 0 or args.poll > args.stale_after:
        raise ValueError("--poll must be positive and no greater than --stale-after")
    if args.request_timeout <= 0 or args.request_timeout > args.stale_after:
        raise ValueError("--request-timeout must be positive and no greater than --stale-after")
    painter: DisplayPainter | None = None
    ws = None
    status_client: StudioStatusClient | None = None
    stop = False

    def request_stop(_signum: int, _frame: Any) -> None:
        nonlocal stop
        stop = True

    old_int = signal.signal(signal.SIGINT, request_stop)
    old_term = signal.signal(signal.SIGTERM, request_stop)
    try:
        if args.display_tool:
            painter = DisplayPainter(load_display_tool(Path(args.display_tool)), args)
            painter.start()
        started = time.monotonic()
        last: ClassifiedStatus | None = None
        while not stop and (args.duration == 0 or time.monotonic() - started <= args.duration):
            sampled_at = time.time()
            status: Any
            age_s: float | None = 0.0
            try:
                if ws is None:
                    ws = ws_factory(args.studio_url, args.request_timeout)
                    status_client = StudioStatusClient(ws)
                if status_client is None:
                    raise StatusProtocolError("status client is not connected")
                status = _status_from_reply(status_client.fetch(timeout_s=args.request_timeout))
                if isinstance(status, Mapping):
                    age_s = _status_age(sampled_at, status)
            except Exception as exc:  # transport failure is fail-closed blue
                status = {"available": False, "error": f"transport: {exc}"}
                age_s = None
                if ws is not None:
                    try:
                        ws.close()
                    except Exception:
                        pass
                    ws = None
                    status_client = None
            classified = classify_status(status, age_s=age_s, stale_after_s=args.stale_after)
            if painter is not None and (last is None or classified.page != last.page):
                painter.update(classified.page)
            print(json.dumps(classified.as_dict(status, sampled_at), sort_keys=True), flush=True)
            last = classified
            if args.duration == 0:
                break
            time.sleep(min(args.poll, max(0.0, args.duration - (time.monotonic() - started))))
        return 0
    finally:
        if ws is not None:
            try:
                ws.close()
            except Exception:
                pass
        if painter is not None:
            painter.close()
        signal.signal(signal.SIGINT, old_int)
        signal.signal(signal.SIGTERM, old_term)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--studio-url", default="http://127.0.0.1:8427")
    parser.add_argument("--request-timeout", type=float, default=2.0)
    parser.add_argument("--stale-after", type=float, default=3.0)
    parser.add_argument("--poll", type=float, default=1.0)
    parser.add_argument("--duration", type=float, default=0.0, help="bounded run time in seconds; default is one sample")
    parser.add_argument("--display-tool", help="explicit path to reviewed pz_display_colors.py; enables bench repaint")
    parser.add_argument("--display-lock", default="/tmp/et7689-color-cycle.lock")
    parser.add_argument("--display-timeout", type=float, default=5.0)
    parser.add_argument("--fpga", default="192.168.137.2")
    parser.add_argument("--host", default="192.168.137.1")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return run(args)
    except (OSError, RuntimeError, StatusProtocolError, TimeoutError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
