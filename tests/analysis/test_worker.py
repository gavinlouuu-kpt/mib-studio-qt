"""Real pinned-engine adapter tests; run with the qualification environment."""
import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import unittest

WORKER = Path(__file__).resolve().parents[2] / "workers/cytoflow/worker.py"
spec = importlib.util.spec_from_file_location("mib_worker", WORKER)
worker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(worker)


def request():
    return {"schema": "mib-cytoflow-polygon/1", "job": "job-1",
            "revision": {name: "cytoflow-1.3.2" if name == "engine" else f"{name}-1"
                         for name in worker.REVISION_FIELDS},
            "vertices": [[0, 0], [2, 0], [2, 2], [0, 2]],
            "events": [{"key": "A/valid/9007199254740993", "x": 1, "y": 1},
                       {"key": "B/valid/9007199254740993", "x": 3, "y": 1},
                       {"key": "A/invalid/9007199254740993", "x": None, "y": 1},
                       {"key": "B/invalid/9007199254740993", "x": 0, "y": 0}]}


class WorkerTest(unittest.TestCase):
    def test_real_engine_exact_keys_and_unknown(self):
        source = request()
        before = copy.deepcopy(source)
        result = worker.evaluate(source)
        self.assertEqual(source, before)
        self.assertEqual(result["revision"], source["revision"])
        self.assertEqual([r["value"] for r in result["rows"]], ["true", "false", "unknown", "true"])
        self.assertEqual([r["key"] for r in result["rows"]], [r["key"] for r in source["events"]])

    def test_winding_and_reorder(self):
        source = request()
        expected = worker.evaluate(source)
        source["vertices"].reverse()
        source["events"].reverse()
        actual = worker.evaluate(source)
        self.assertEqual(actual["rows"], list(reversed(expected["rows"])))

    def test_concave_boundary_and_notch(self):
        source = request()
        source["vertices"] = [[0, 0], [3, 0], [3, 1], [1, 1], [1, 3], [0, 3]]
        source["events"] = [{"key": str(i), "x": x, "y": y} for i, (x, y) in
                            enumerate([(0.5, 2), (2, 2), (1, 2), (1, 1), (3, 0)])]
        self.assertEqual([r["value"] for r in worker.evaluate(source)["rows"]],
                         ["true", "false", "true", "true", "true"])

    def test_empty_and_all_unknown(self):
        source = request()
        source["events"] = []
        self.assertEqual(worker.evaluate(source)["rows"], [])
        source["events"] = [{"key": "unknown", "x": None, "y": None}]
        self.assertEqual(worker.evaluate(source)["rows"][0]["value"], "unknown")

    def test_invalid_geometry(self):
        invalid = [[[0, 0], [2, 2], [0, 2], [2, 0]],
                   [[0, 0], [1, 1], [2, 2]],
                   [[0, 0], [2, 0], [1, 0], [2, 2]],
                   [[0, 0], [float("nan"), 0], [1, 1]],
                   [[0, 0], [1, 0]], [[0, 0], [1, 0], [1, 0], [0, 1]]]
        for vertices in invalid:
            with self.subTest(vertices=vertices):
                source = request()
                source["vertices"] = vertices
                with self.assertRaises(ValueError):
                    worker.evaluate(source)

    def test_keys_and_values_fail_closed(self):
        for bad in [True, "1", float("inf"), 1e101]:
            source = request()
            source["events"][0]["x"] = bad
            with self.assertRaises(ValueError):
                worker.evaluate(source)
        source = request()
        source["events"][1]["key"] = source["events"][0]["key"]
        with self.assertRaises(ValueError):
            worker.evaluate(source)
        source = request()
        source["events"][0]["key"] = 9007199254740993
        with self.assertRaises(ValueError):
            worker.evaluate(source)

    def test_revision_schema_and_budget(self):
        source = request()
        source["revision"]["mechanics"] = ""
        with self.assertRaises(ValueError):
            worker.evaluate(source)
        source = request()
        source["expression"] = "arbitrary expression is forbidden"
        with self.assertRaises(ValueError):
            worker.evaluate(source)
        source = request()
        source["events"] = source["events"] * (worker.MAX_EVENTS // 4 + 1)
        with self.assertRaises(ValueError):
            worker.evaluate(source)

    def test_real_subprocess_and_duplicate_json(self):
        result = subprocess.run([sys.executable, str(WORKER)], input=json.dumps(request()),
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["job"], "job-1")
        result = subprocess.run([sys.executable, str(WORKER)],
                                input='{"job":"a","job":"b"}',
                                capture_output=True, text=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "")
        self.assertIn("duplicate JSON", result.stderr)


if __name__ == "__main__":
    unittest.main()
