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
import math
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


def _valid_bool(mapping: Mapping[str, Any], key: str) -> bool:
    """Validate a boolean leaf when it is part of the current DTO."""
    return key not in mapping or type(mapping[key]) is bool


def _validate_status_shape(status: Mapping[str, Any]) -> str | None:
    """Validate the fields needed to make a no-fault decision.

    The bridge DTO has optional blocks, but a status display cannot safely
    infer health from an available object missing its identity/fault blocks.
    Unknown identity is therefore blue.  Unknown future fields are ignored;
    known boolean leaves may not be stringly typed.
    """
    for field in ("mode", "core", "led", "link"):
        if not isinstance(status.get(field), Mapping):
            return f"malformed.{field}"
    mode = status["mode"]
    if mode.get("name") not in {"align", "run", "unknown"}:
        return "unknown.mode"
    if mode.get("name") == "unknown":
        return "unknown.mode"
    for key in ("run_set", "idle", "service"):
        if not _valid_bool(mode, key):
            return f"malformed.mode.{key}"
    core = status["core"]
    for key in ("build_match", "profile_match"):
        if core.get(key) not in {"match", "mismatch", "unknown"}:
            return f"malformed.core.{key}"
        if core[key] == "unknown":
            return f"unknown.core.{key}"
    led = status["led"]
    if type(led.get("guard_fault")) is not bool:
        return "malformed.led.guard_fault"
    for key in ("on",):
        if not _valid_bool(led, key):
            return f"malformed.led.{key}"
    link = status["link"]
    for key in ("rates_valid", "ingress_errors_warn", "bad_frames_warn", "dropped_warn", "bridge_active"):
        if not _valid_bool(link, key):
            return f"malformed.link.{key}"
    heal = link.get("rx_heal")
    if not isinstance(heal, Mapping):
        return "malformed.link.rx_heal"
    for key in ("present", "gave_up", "v2", "fs_present"):
        if not _valid_bool(heal, key):
            return f"malformed.link.rx_heal.{key}"
    if type(heal.get("gave_up")) is not bool:
        return "malformed.link.rx_heal.gave_up"
    if "error" in status and not isinstance(status["error"], str):
        return "malformed.error"
    return None


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
    if status.get("stale") is True:
        return ClassifiedStatus("blue", 8, BLUE["label"], "stale", False, True)
    for field in ("mode", "core", "led", "link", "results", "storage"):
        if field in status and not isinstance(status[field], Mapping):
            return ClassifiedStatus("blue", 8, BLUE["label"], f"malformed.{field}", False, available)
    shape_error = _validate_status_shape(status)
    if shape_error:
        return ClassifiedStatus("blue", 8, BLUE["label"], shape_error, False, available)
    if age_s is None or not isinstance(age_s, (int, float)) or not math.isfinite(age_s) or age_s < 0 or age_s > stale_after_s:
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
    return connect(ws_url, open_timeout=timeout_s, close_timeout=timeout_s, max_size=1024 * 1024)


def load_display_tool(path: Path) -> Any:
    if not path.is_file():
        raise FileNotFoundError(path)
    spec = importlib.util.spec_from_file_location("yofo_et7689_display_tool", path)
    if spec is None or spec.loader is None:
        raise ImportError(f"cannot import display tool {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    try:
        spec.loader.exec_module(module)
    except BaseException:
        sys.modules.pop(spec.name, None)
        raise
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
            if self.original_page is not None:
                try:
                    # Always issue and verify the restore, even when the
                    # cached page says it is already selected: a failed
                    # readback may have left the display in an unknown page.
                    self.display.write_register(self.module.PAGE_REGISTER, self.original_page)
                    actual = int.from_bytes(self.display.read_register(self.module.PAGE_REGISTER), "big")
                    if actual != self.original_page:
                        raise StatusProtocolError(f"display page restore {actual} != {self.original_page}")
                    self.current_page = self.original_page
                except Exception as exc:
                    errors.append(f"page restore: {exc}")
            if self.original_brightness is not None:
                try:
                    # Independent from page restoration so a page readback
                    # failure cannot skip the brightness restore.
                    self.module_value_restore(self.module.BACKLIGHT_REGISTER, self.original_brightness, "brightness")
                except Exception as exc:
                    errors.append(f"brightness restore: {exc}")
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
    """Extract explicit freshness fields; invalid/future values fail closed."""
    if status.get("stale") is True:
        return float("inf")
    if "age_ms" in status:
        age_ms = status.get("age_ms")
        if type(age_ms) not in (int, float):
            return float("inf")
        value = float(age_ms)
        return value / 1000.0 if math.isfinite(value) and value >= 0 else float("inf")
    for key in ("sampled_at", "timestamp"):
        if key not in status:
            continue
        value = status.get(key)
        if type(value) not in (int, float):
            return float("inf")
        seconds = float(value)
        if not math.isfinite(seconds) or seconds > sampled_at:
            return float("inf")
        return max(0.0, sampled_at - seconds)
    return 0.0


def run(
    args: argparse.Namespace,
    *,
    ws_factory: Callable[[str, float], Any] = connect_studio,
    clock: Callable[[], float] | None = None,
    sleep_fn: Callable[[float], None] | None = None,
) -> int:
    if args.duration < 0 or args.duration > 3600:
        raise ValueError("--duration must be between 0 and 3600 seconds")
    if args.poll <= 0 or args.poll > args.stale_after:
        raise ValueError("--poll must be positive and no greater than --stale-after")
    if args.request_timeout <= 0 or args.request_timeout > args.stale_after:
        raise ValueError("--request-timeout must be positive and no greater than --stale-after")
    clock = clock or time.monotonic
    sleep_fn = sleep_fn or time.sleep
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
        started = clock()
        last: ClassifiedStatus | None = None
        last_valid_at: float | None = None
        while not stop and (args.duration == 0 or clock() - started < args.duration):
            sampled_at = time.time()
            now = clock()
            if last_valid_at is not None and now - last_valid_at >= args.stale_after:
                stale_status = {"available": True, "stale": True, "error": "local freshness deadline expired"}
                stale = classify_status(stale_status, age_s=float("inf"), stale_after_s=args.stale_after)
                if painter is not None and (last is None or stale.page != last.page):
                    painter.update(stale.page)
                print(json.dumps(stale.as_dict(stale_status, sampled_at), sort_keys=True), flush=True)
                last = stale
                last_valid_at = None
            status: Any
            age_s: float | None = 0.0
            try:
                duration_remaining = args.duration - (now - started) if args.duration else args.request_timeout
                freshness_remaining = args.stale_after if last_valid_at is None else args.stale_after - (now - last_valid_at)
                request_budget = min(args.request_timeout, duration_remaining, freshness_remaining)
                if request_budget <= 0:
                    break
                operation_deadline = now + request_budget
                if ws is None:
                    ws = ws_factory(args.studio_url, request_budget)
                    status_client = StudioStatusClient(ws)
                if status_client is None:
                    raise StatusProtocolError("status client is not connected")
                fetch_budget = min(request_budget, max(0.0, operation_deadline - clock()))
                if fetch_budget <= 0:
                    raise TimeoutError("status freshness deadline expired before fetch")
                status = _status_from_reply(status_client.fetch(timeout_s=fetch_budget))
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
            after_fetch = clock()
            if classified.fresh and classified.available is True:
                last_valid_at = after_fetch
            elif classified.color == "blue":
                last_valid_at = None
            if painter is not None and (last is None or classified.page != last.page):
                painter.update(classified.page)
            print(json.dumps(classified.as_dict(status, sampled_at), sort_keys=True), flush=True)
            last = classified
            if args.duration == 0:
                break
            now = clock()
            duration_remaining = args.duration - (now - started)
            freshness_remaining = args.stale_after if last_valid_at is None else args.stale_after - (now - last_valid_at)
            sleep_budget = min(args.poll, max(0.0, duration_remaining), max(0.0, freshness_remaining))
            if sleep_budget > 0:
                sleep_fn(sleep_budget)
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
