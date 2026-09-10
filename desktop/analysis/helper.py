"""Development-only private-pipe endpoint for Toolkit-owned calculations.

Reads only the optional bundle manifest; no dataset, output, network listener or
hardware methods. Production launch awaits native ledger integration (issue #399).
"""

from __future__ import annotations

import hashlib
import json
import math
import struct
import sys
from typing import BinaryIO

PROTOCOL = {"major": 0, "minor": 1}
TOOLKIT_VERSION = "0.1.0"
MAX_MESSAGE = 1 << 20
MAX_ROWS = 4096
MAX_ID = (1 << 53) - 1


class ProtocolError(ValueError):
    pass


def read_exact(stream: BinaryIO, size: int) -> bytes:
    result = bytearray()
    while len(result) < size:
        chunk = stream.read(size - len(result))
        if not chunk:
            raise ProtocolError("Truncated message")
        result.extend(chunk)
    return bytes(result)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ProtocolError("Duplicate JSON key")
        result[key] = value
    return result


def reject_constant(value):
    raise ProtocolError("Non-finite JSON number")


def read_message(stream: BinaryIO):
    first = stream.read(1)
    if not first:
        return None
    size = struct.unpack(">I", first + read_exact(stream, 3))[0]
    if not 0 < size <= MAX_MESSAGE:
        raise ProtocolError("Message exceeds control budget")
    try:
        value = json.loads(
            read_exact(stream, size).decode("utf-8"),
            object_pairs_hook=unique_object,
            parse_constant=reject_constant,
        )
    except (UnicodeError, ValueError, RecursionError) as exc:
        raise ProtocolError("Invalid JSON message") from exc
    if not isinstance(value, dict):
        raise ProtocolError("Expected object")
    return value


def write_message(stream: BinaryIO, value):
    payload = json.dumps(value, allow_nan=False, separators=(",", ":")).encode("utf-8")
    if len(payload) > MAX_MESSAGE:
        raise ProtocolError("Result exceeds control budget")
    stream.write(struct.pack(">I", len(payload)) + payload)
    stream.flush()


def integer(value, low, high):
    if type(value) is not int or not low <= value <= high:
        raise ProtocolError("Integer outside permitted range")
    return value


def values(column):
    if not isinstance(column, list) or len(column) > MAX_ROWS:
        raise ProtocolError("Column exceeds 4096-row budget")
    for value in column:
        if type(value) not in (int, float) or not math.isfinite(value):
            raise ProtocolError("Columns require finite numbers")
    return column


class Session:
    def __init__(self, bundle=None):
        self.generation = None
        self.last_request = 0
        self.bundle = bundle

    def dispatch(self, request):
        if set(request) != {
            "protocol",
            "request_id",
            "operation_id",
            "generation",
            "method",
            "params",
        }:
            raise ProtocolError("Invalid envelope")
        protocol = request["protocol"]
        if (
            not isinstance(protocol, dict)
            or set(protocol) != {"major", "minor"}
            or any(type(value) is not int for value in protocol.values())
            or protocol != PROTOCOL
        ):
            raise ProtocolError("Protocol mismatch")
        request_id = integer(request["request_id"], 1, MAX_ID)
        if request_id <= self.last_request:
            raise ProtocolError("Request IDs must increase")
        self.last_request = request_id
        generation = integer(request["generation"], 0, MAX_ID)
        operation = request["operation_id"]
        if not isinstance(operation, str) or not 1 <= len(operation) <= 64:
            raise ProtocolError("Invalid operation ID")
        method, params = request["method"], request["params"]
        if not isinstance(method, str) or not isinstance(params, dict):
            raise ProtocolError("Invalid method or parameters")
        result = self.calculate(method, params, generation)
        return {
            "protocol": PROTOCOL,
            "request_id": request_id,
            "operation_id": operation,
            "generation": generation,
            "status": "completed",
            "result": result,
        }

    def calculate(self, method, params, generation):
        if self.generation is None:
            expected = {"toolkit_version": TOOLKIT_VERSION}
            if self.bundle:
                expected["bundle_sha256"] = self.bundle["bundle_sha256"]
            if method != "handshake" or params != expected:
                raise ProtocolError("Matching handshake required")
            import biowork_toolkit

            if biowork_toolkit.__version__ != TOOLKIT_VERSION:
                raise ProtocolError("Toolkit version mismatch")
            self.generation = generation
            hello = {
                "toolkit_version": TOOLKIT_VERSION,
                "distribution": "development-only",
                "production_ready": False,
                "capabilities": ["histogram_page", "kde_page"],
                "max_rows": MAX_ROWS,
                "max_message_bytes": MAX_MESSAGE,
            }
            if self.bundle:
                hello.update(self.bundle)
            return hello
        if method == "select_generation":
            if params or generation <= self.generation:
                raise ProtocolError("Generation must advance")
            self.generation = generation
            return {}
        if generation != self.generation:
            raise ProtocolError("Stale dataset generation")
        if method == "histogram_page":
            if set(params) != {"values", "metric", "bins"}:
                raise ProtocolError("Invalid histogram parameters")
            metric = params["metric"]
            if not isinstance(metric, str) or not 1 <= len(metric) <= 256:
                raise ProtocolError("Invalid metric")
            column = values(params["values"])
            bins = integer(params["bins"], 1, 256)
            from biowork_toolkit.analysis import build_histogram

            return build_histogram(
                column, metric=metric, bin_count=bins, max_values=MAX_ROWS
            ).model_dump(mode="json")
        if method == "kde_page":
            if set(params) != {"x", "y", "columns", "rows", "bandwidth"}:
                raise ProtocolError("Invalid KDE parameters")
            x, y = values(params["x"]), values(params["y"])
            if len(x) != len(y):
                raise ProtocolError("KDE columns must have equal lengths")
            columns = integer(params["columns"], 2, 128)
            rows = integer(params["rows"], 2, 128)
            bandwidth = params["bandwidth"]
            if (
                type(bandwidth) not in (int, float)
                or not math.isfinite(bandwidth)
                or not 0.5 <= bandwidth <= 8
            ):
                raise ProtocolError("Bandwidth outside bounded work budget")
            from biowork_toolkit.analysis import build_kde_grid

            return build_kde_grid(
                x,
                y,
                columns=columns,
                rows=rows,
                bandwidth=bandwidth,
                max_points=MAX_ROWS,
            ).model_dump(mode="json")
        raise ProtocolError("Unsupported method")


def main():
    # Windows inherited CRT pipes must not translate binary framing bytes.
    if sys.platform == "win32":
        import msvcrt
        import os

        msvcrt.setmode(sys.stdin.fileno(), os.O_BINARY)
        msvcrt.setmode(sys.stdout.fileno(), os.O_BINARY)
    try:
        bundle = None
        if len(sys.argv) != 1:
            if len(sys.argv) != 3 or sys.argv[1] != "--bundle-manifest":
                raise ProtocolError("Invalid launch arguments")
            # The native supervisor verifies every bundle file against its
            # externally pinned manifest before launch. Echo that identity;
            # this is not a replacement for the supervisor's trust check.
            with open(sys.argv[2], "rb") as stream:
                raw = stream.read(MAX_MESSAGE + 1)
            if len(raw) > MAX_MESSAGE:
                raise ProtocolError("Oversized bundle manifest")
            manifest = json.loads(raw, object_pairs_hook=unique_object)
            if (
                manifest["schema"] != 1
                or manifest["toolkit_version"] != TOOLKIT_VERSION
            ):
                raise ProtocolError("Bundle version mismatch")
            if manifest["distribution"] not in ("development", "production"):
                raise ProtocolError("Unknown distribution")
            bundle = {
                "bundle_sha256": hashlib.sha256(raw).hexdigest(),
                "wheel_sha256": manifest["files"][manifest["toolkit_wheel"]]["sha256"],
                "distribution": manifest["distribution"],
                "production_ready": manifest["distribution"] == "production",
            }
        session = Session(bundle)
        while (request := read_message(sys.stdin.buffer)) is not None:
            write_message(sys.stdout.buffer, session.dispatch(request))
    except (
        ProtocolError,
        ValueError,
        TypeError,
        OverflowError,
        RecursionError,
        ImportError,
        BrokenPipeError,
        OSError,
        KeyError,
    ):
        # Fail closed without emitting an uncorrelated result or traceback/data.
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
