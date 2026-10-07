#!/usr/bin/env python3
"""Offline HDF5 -> native FCS -> FlowIO round-trip; exit 77 without FlowIO."""

from __future__ import annotations

import argparse
import csv
import datetime
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import warnings


def run_checked(command: list[str], timeout: int = 45) -> None:
    result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f"{command[0]} exited {result.returncode}:\n{result.stdout}{result.stderr}")


def check_file(flowio, path: Path, contract: int, observations: list[int]) -> None:
    common = ["Area_um2", "Area_px2", "Deformability", "AreaRatio"]
    names = common + (
        ["RingRatio", "BrightQ1", "BrightQ2", "BrightQ3", "BrightQ4"]
        if contract == 1 else
        ["LaplacianVar", "BrightQ1", "BrightQ2", "BrightQ3", "BrightQ4"]
        if contract == 2 else
        ["LaplacianVar", "BrightMean", "BrightVar", "Pixels", "Blemishes"]
    ) + ["Time"]
    with warnings.catch_warnings():
        warnings.simplefilter("error")
        data = flowio.FlowData(str(path))
    assert data.version == "3.1"
    assert data.event_count == len(observations)
    assert data.pnn_labels == names, data.pnn_labels
    assert data.text["datatype"] == "F" and data.text["byteord"] == "1,2,3,4"
    assert data.text["mode"] == "L" and float(data.text["timestep"]) == 1.0
    assert int(data.text["mib_contract"]) == contract
    assert data.text["fil"] == path.name.replace(".fcs", ".h5")
    assert data.text["mib_version"]
    assert float(data.text["mib_pixel_to_micron"]) == 0.5
    datetime.datetime.fromisoformat(data.text["mib_export_time"].replace("Z", "+00:00"))

    for p in range(1, len(names) + 1):
        assert data.text[f"p{p}b"] == "32"
        assert data.text[f"p{p}e"] == "0,0"
        assert data.text[f"p{p}s"]
        assert float(data.text[f"p{p}r"]) > 0

    base_ns = 1700000000000000000
    indices = [42, 9007199254740993, 8]
    areas = [100.25, 200.5, 50.0]
    deformations = [0.125, 0.25, 0.5]
    with path.with_name(path.stem + "_event_map.csv").open(newline="") as stream:
        reader = csv.DictReader(stream)
        assert {"fcs_event_index", "source_frame_index", "object_id", "timestamp_ns", "event_mode"}.issubset(
            reader.fieldnames or []
        )
        mapping = list(reader)
    assert len(mapping) == len(observations)
    for event, observation in enumerate(observations):
        values = {
            "Area_um2": areas[observation] * 0.25,
            "Area_px2": areas[observation],
            "Deformability": deformations[observation],
            "AreaRatio": 1.1 + observation * 0.1,
            "RingRatio": 0.5 + observation * 0.1,
            "LaplacianVar": 42.0 * (observation + 1),
            "BrightMean": 112.5 + observation,
            "BrightVar": 33.25 + observation,
            "Pixels": 250 + observation,
            "Blemishes": observation,
            "Time": float(observation - observations[0]),
            **{f"BrightQ{q}": q * 10.0 + observation for q in range(1, 5)},
        }
        for channel, name in enumerate(names):
            actual = data.events[event * len(names) + channel]
            assert math.isclose(actual, values[name], rel_tol=1e-6, abs_tol=1e-6), (
                path, event, name, actual, values[name]
            )
            assert abs(actual) < float(data.text[f"p{channel + 1}r"])
        row = mapping[event]
        assert int(row["fcs_event_index"]) == event
        assert int(row["source_frame_index"]) == indices[observation]
        assert int(row["object_id"]) == 7
        assert int(row["timestamp_ns"]) == base_ns + observation * 1000000000
        assert row["event_mode"] == "detection"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture-generator", required=True, type=Path)
    parser.add_argument("--native-cli", required=True, type=Path)
    args = parser.parse_args()
    try:
        import flowio
    except ImportError:
        print("SKIP: FlowIO is not importable (optional offline reader check)")
        return 77

    repo = Path(__file__).resolve().parents[1]
    runtime = repo / "data"
    runtime.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="fcs-flowio-", dir=runtime) as temporary:
        root = Path(temporary)
        fixtures = root / "fixtures"
        run_checked([str(args.fixture_generator.resolve()), "--fixtures", str(fixtures)])
        for value in ("abc", "1garbage", "nan", "inf", "0", "-1"):
            output = root / "invalid-calibration"
            result = subprocess.run(
                [str(args.native_cli.resolve()), "--input", str(fixtures / "contract1.h5"),
                 "--output", str(output), "--format", "fcs", "--pixel-to-micron", value],
                capture_output=True, text=True, timeout=15,
            )
            assert result.returncode == 2, (value, result.returncode, result.stderr)
            assert not output.exists(), "invalid CLI input must fail before starting an export"
        for contract in (1, 2, 3):
            source = fixtures / f"contract{contract}.h5"
            for selection, observations in ((None, [0, 1]), ("both", [0, 1, 2]), ("invalid", [2])):
                output = root / f"export-{contract}-{selection or 'default'}"
                command = [sys.executable, str(repo / "scripts" / "export_hdf5.py"),
                           "-i", str(source), "-o", str(output), "--format", "fcs",
                           "--native-cli", str(args.native_cli.resolve()), "--pixel-to-micron", "0.5"]
                if selection:
                    command += ["--frame-type", selection]
                run_checked(command)
                check_file(flowio, output / source.stem / f"{source.stem}.fcs", contract, observations)
        output = root / "export-empty"
        run_checked([str(args.native_cli.resolve()), "--input", str(fixtures / "empty.h5"),
                     "--output", str(output), "--format", "fcs", "--pixel-to-micron", "0.5"])
        check_file(flowio, output / "empty" / "empty.fcs", 1, [])
    print(f"FlowIO {flowio.__version__}: 9 HDF5/CLI round-trips and empty dataset passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
