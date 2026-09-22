import json
from pathlib import Path
import sys

import h5py
import numpy as np
import pyarrow.parquet as pq
import pytest
sys.path.insert(0, str(Path(__file__).resolve().parent))
from prepare_focus_benchmark import create, indices
from run_focus_benchmark import run
from publish_focus_benchmark import publish


def recording(path, frames):
    path.parent.mkdir(parents=True, exist_ok=True)
    with h5py.File(path, "w") as f:
        f.create_dataset("recorded_frames/images", data=frames)
        metadata = np.zeros(len(frames), dtype=[("timestampNs", "<u8")])
        metadata["timestampNs"] = np.arange(len(frames))*1000
        f.create_dataset("recorded_frames/metadata", data=metadata)


def test_roundtrip_exclusions_and_native_benchmark(tmp_path):
    source = tmp_path / "input"
    frames = np.random.default_rng(21).integers(0, 256, (18, 24, 32), dtype=np.uint8)
    selected = indices(18, 6)
    frames[selected[1]] = frames[selected[0]]
    recording(source / "Test_in different focus/40v.h5", frames)
    recording(source / "Test_in different focus/40v_remasked.h5", frames)
    (source / "Test_in different focus/45v.h5").write_bytes(b"bad hdf5")
    output = tmp_path / "dataset"
    manifest = create(source, output, 6, 3)
    assert len(manifest["exclusions"]) == 2
    assert manifest["total_retained"] == 5
    item = manifest["recordings"][0]
    assert set(item["background_frame_indices"]).isdisjoint(selected)
    assert item["duplicate_frame_indices"] == [selected[1]]
    assert pq.read_table(output/item["parquet"]).num_rows == 5
    result = run(output, output / "benchmark")
    assert result["total_input_frames"] == 5
    assert len(result["summary"]) == 8
    dry = publish(output, "test/focus-benchmark", dry_run=True)
    assert dry["private"] is True and dry["files"] == 9
    with pytest.raises(FileExistsError): create(source, output, 6, 3)
    with pytest.raises(ValueError): create(source, source / "output", 6, 3)
    # Integrity faults must never be silently benchmarked.
    background = output/item["background"]
    background.write_bytes(background.read_bytes()+b"changed")
    with pytest.raises(ValueError, match="hash mismatch"):
        run(output, output / "corrupt-run")


def test_no_disjoint_background_is_explicit_exclusion(tmp_path):
    source = tmp_path / "input"
    recording(source / "Test/40v.h5", np.zeros((2, 8, 8), np.uint8))
    manifest = create(source, tmp_path / "output", 2, 3)
    assert manifest["status"] == "empty"
    assert "disjoint" in manifest["exclusions"][0]["reason"]


def test_zero_samples_rejected_before_creating_output(tmp_path):
    with pytest.raises(ValueError): create(tmp_path / "input", tmp_path / "out", 0, 3)
    assert not (tmp_path / "out").exists()
