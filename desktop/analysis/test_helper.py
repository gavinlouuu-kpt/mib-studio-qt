"""Private pipe integration tests; run with the pinned Toolkit environment."""

import io
import json
import struct
import subprocess
import sys
import unittest
from pathlib import Path

from biowork_toolkit.analysis import build_histogram, build_kde_grid

HELPER = Path(__file__).with_name("helper.py")
PROTOCOL = {"major": 0, "minor": 1}


def envelope(method, params, request_id=1, generation=1):
    return {
        "protocol": PROTOCOL,
        "request_id": request_id,
        "operation_id": f"op-{request_id}",
        "generation": generation,
        "method": method,
        "params": params,
    }


def frame(value):
    body = json.dumps(value).encode()
    return struct.pack(">I", len(body)) + body


HANDSHAKE = envelope("handshake", {"toolkit_version": "0.1.0"})
HISTOGRAM = envelope(
    "histogram_page", {"values": [1, 2, 2, 5], "metric": "area", "bins": 4}, 2
)


def run_bytes(payload):
    with subprocess.Popen(
        [sys.executable, "-I", str(HELPER)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    ) as child:
        try:
            output, errors = child.communicate(payload, timeout=10)
        except subprocess.TimeoutExpired:
            child.kill()
            child.communicate()
            raise AssertionError("Helper exceeded process watchdog") from None
        replies = []
        stream = io.BytesIO(output)
        while header := stream.read(4):
            if len(header) != 4:
                raise AssertionError("Partial reply header")
            size = struct.unpack(">I", header)[0]
            if not 0 < size <= 1 << 20:
                raise AssertionError("Reply exceeds budget")
            body = stream.read(size)
            if len(body) != size:
                raise AssertionError("Partial reply body")
            replies.append(json.loads(body))
        return child.returncode, replies, errors


class HelperTests(unittest.TestCase):
    def test_histogram_and_kde_match_toolkit_over_real_pipes(self):
        kde = envelope(
            "kde_page",
            {
                "x": [1, 2, 2, 5],
                "y": [2, 3, 1, 4],
                "columns": 8,
                "rows": 6,
                "bandwidth": 2,
            },
            3,
        )
        code, replies, errors = run_bytes(
            b"".join(map(frame, [HANDSHAKE, HISTOGRAM, kde]))
        )
        self.assertEqual((code, errors), (0, b""))
        self.assertEqual(len(replies), 3)
        self.assertFalse(replies[0]["result"]["production_ready"])
        self.assertEqual(
            replies[1]["result"],
            build_histogram(
                [1, 2, 2, 5], metric="area", bin_count=4, max_values=4096
            ).model_dump(mode="json"),
        )
        self.assertEqual(
            replies[2]["result"],
            build_kde_grid(
                [1, 2, 2, 5],
                [2, 3, 1, 4],
                columns=8,
                rows=6,
                bandwidth=2,
                max_points=4096,
            ).model_dump(mode="json"),
        )
        for index, reply in enumerate(replies, 1):
            self.assertEqual(reply["request_id"], index)
            self.assertEqual(reply["operation_id"], f"op-{index}")
            self.assertEqual(reply["generation"], 1)

    def test_generation_change_rejects_old_result_request(self):
        select = envelope("select_generation", {}, 2, 2)
        stale = envelope("histogram_page", HISTOGRAM["params"], 3, 1)
        code, replies, errors = run_bytes(
            b"".join(map(frame, [HANDSHAKE, select, stale]))
        )
        self.assertEqual((code, len(replies), errors), (2, 2, b""))

    def test_replay_and_unhandshaken_work_are_rejected(self):
        for requests in ([HISTOGRAM], [HANDSHAKE, HANDSHAKE]):
            with self.subTest(requests=requests):
                code, replies, errors = run_bytes(b"".join(map(frame, requests)))
                self.assertEqual(
                    (code, len(replies), errors), (2, len(requests) - 1, b"")
                )

    def test_protocol_version_and_toolkit_mismatch(self):
        for request in (
            dict(HANDSHAKE, protocol={"major": False, "minor": True}),
            dict(HANDSHAKE, protocol={"major": 1, "minor": 0}),
            dict(HANDSHAKE, params={"toolkit_version": "9.0.0"}),
        ):
            code, replies, errors = run_bytes(frame(request))
            self.assertEqual((code, replies, errors), (2, [], b""))

    def test_invalid_or_excessive_work_has_no_success_reply(self):
        cases = [
            dict(HISTOGRAM, method="open_file"),
            dict(HISTOGRAM, request_id=True),
            dict(HISTOGRAM, generation=-1),
            dict(HISTOGRAM, params=dict(HISTOGRAM["params"], values=[1] * 4097)),
            dict(HISTOGRAM, params=dict(HISTOGRAM["params"], bins=257)),
            dict(HISTOGRAM, params=dict(HISTOGRAM["params"], values=[True])),
            dict(HISTOGRAM, params=dict(HISTOGRAM["params"], values=[float("nan")])),
            dict(HISTOGRAM, params=dict(HISTOGRAM["params"], values=[1e308, -1e308])),
            envelope(
                "kde_page",
                {"x": [1], "y": [], "columns": 8, "rows": 8, "bandwidth": 2},
                2,
            ),
            envelope(
                "kde_page",
                {"x": [], "y": [], "columns": 129, "rows": 8, "bandwidth": 2},
                2,
            ),
            envelope(
                "kde_page",
                {"x": [], "y": [], "columns": 8, "rows": 8, "bandwidth": 9},
                2,
            ),
        ]
        for request in cases:
            with self.subTest(request=request):
                code, replies, errors = run_bytes(frame(HANDSHAKE) + frame(request))
                self.assertEqual((code, len(replies), errors), (2, 1, b""))

    def test_malformed_and_truncated_framing(self):
        duplicate = b'{"x":1,"x":2}'
        cases = [
            b"\x00",
            struct.pack(">I", (1 << 20) + 1),
            struct.pack(">I", 0),
            struct.pack(">I", 20) + b"{}",
            struct.pack(">I", 1) + b"\xff",
            struct.pack(">I", len(duplicate)) + duplicate,
            frame([]),
        ]
        for payload in cases:
            with self.subTest(payload=payload):
                self.assertEqual(run_bytes(payload), (2, [], b""))

    def test_maximum_page_and_repeated_process_exit(self):
        full = dict(
            HISTOGRAM, params=dict(HISTOGRAM["params"], values=list(range(4096)))
        )
        for _ in range(10):
            code, replies, errors = run_bytes(frame(HANDSHAKE) + frame(full))
            self.assertEqual((code, len(replies), errors), (0, 2, b""))
            self.assertEqual(replies[1]["result"]["total"], 4096)

    def test_idle_eof_and_forced_shutdown_reap_child(self):
        self.assertEqual(run_bytes(b""), (0, [], b""))
        with subprocess.Popen(
            [sys.executable, "-I", str(HELPER)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        ) as child:
            try:
                # A parent can interrupt a stalled private-pipe read without
                # sending a cancellation request behind a calculation.
                child.stdin.write(b"\x00")
                child.stdin.flush()
                child.kill()
                child.communicate(timeout=10)
                self.assertIsNotNone(child.returncode)
            finally:
                if child.poll() is None:
                    child.kill()
                    child.communicate(timeout=10)


if __name__ == "__main__":
    unittest.main()
