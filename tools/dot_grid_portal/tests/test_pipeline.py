import json
from pathlib import Path
import sys
import zipfile
import ezdxf
import gdstk
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "generator"))
from pipeline import inspect, generate, digest, Registry


def drawing(path, units=13):
    d = ezdxf.new("R2010")
    d.header["$INSUNITS"] = units
    d.modelspace().add_lwpolyline(
        [(0, 0), (6000, 0), (6000, 6000), (0, 6000)], close=True
    )
    d.modelspace().add_line((1000, 3000), (5000, 3000))
    d.layers.new("NO_DOT")
    d.modelspace().add_lwpolyline([(1000,2900),(5000,2900),(5000,3100),(1000,3100)], close=True, dxfattribs={"layer":"NO_DOT"})
    d.saveas(path)
    return path


def test_real_dxf_map_roundtrip(tmp_path):
    source = drawing(tmp_path / "device.dxf")
    out = tmp_path / "out"
    i = inspect(source, "um", out)
    meta = {
        "inspection": i,
        "id": "test-chip-v1",
        "name": "Test chip",
        "seed": 9,
        "designs": [],
    }
    result = generate(out / "normalized.dxf", meta, out)
    registry = Registry.load(str(out / "registry.json"))
    assert registry.find("test-chip-v1").seed == 9
    assert registry.find("test-chip-v1").codec_contract == 1
    d = ezdxf.readfile(out / "design_with_dotgrid.dxf")
    assert len(d.modelspace().query("LINE")) == 1
    assert len(d.modelspace().query("LWPOLYLINE")) == 2
    assert len(d.modelspace().query("CIRCLE")) == result["manifest"]["dot_count"]
    assert d.units == 13
    gds = gdstk.read_gds(out / "dotgrid_layer.gds")
    assert len(gds["DOTGRID"].references) == result["manifest"]["dot_count"]
    assert {p.layer for p in gds["DOT"].polygons} == {10}
    with zipfile.ZipFile(out / "mib-map.zip") as z:
        assert set(z.namelist()) == {
            "registry.json",
            "codebook.json",
            "manifest.json",
            "README.txt",
        }
        manifest = json.loads(z.read("manifest.json"))
        assert manifest["fabrication_approved"] is False
        for n, h in manifest["files"].items():
            assert digest(out / n) == h
    (out / "normalized.dxf").write_text("tampered", encoding="utf-8")
    with pytest.raises(ValueError, match="checksum"):
        generate(out / "normalized.dxf", meta, out)


def test_units_and_negative_origin(tmp_path):
    p = tmp_path / "mm.dxf"
    d = ezdxf.new()
    d.modelspace().add_lwpolyline([(-3, -3), (3, -3), (3, 3), (-3, 3)], close=True)
    d.layers.new("NO_DOT")
    d.modelspace().add_circle((0,0),.3,dxfattribs={"layer":"NO_DOT"})
    d.saveas(p)
    i = inspect(p, "mm", tmp_path / "out")
    assert i["extent_um"] == [6000, 6000]
    assert i["transform"] == {"scale_to_um": 1000.0, "translation_um": [3000.0, 3000.0]}
    assert ezdxf.readfile(tmp_path / "out/normalized.dxf").units == 13


def test_annotations_preserved_but_not_device_bounds(tmp_path):
    p = drawing(tmp_path / "bad.dxf")
    d = ezdxf.readfile(p)
    d.modelspace().add_text("not geometry")
    d.saveas(p)
    i = inspect(p, "um", tmp_path / "out")
    assert i["annotation_count"] == 1
    assert len(ezdxf.readfile(tmp_path / "out/normalized.dxf").modelspace().query("TEXT")) == 1


def test_excess_extent_rejected(tmp_path):
    p = drawing(tmp_path / "wrong-units.dxf")
    with pytest.raises(ValueError, match="extent"):
        inspect(p, "mm", tmp_path / "out")


def test_missing_dwg_converter_fails_closed(tmp_path, monkeypatch):
    monkeypatch.delenv("CAD_ODA_EXECUTABLE", raising=False)
    p = tmp_path / "input.dwg"
    p.write_bytes(b"AC1032")
    monkeypatch.setattr("cad_import.shutil.which", lambda _: None)
    with pytest.raises(ValueError, match="conversion unavailable"):
        inspect(p, "um", tmp_path / "out")


def test_bulges_rejected(tmp_path):
    p = tmp_path / "bulge.dxf"
    d = ezdxf.new()
    d.modelspace().add_lwpolyline(
        [(0, 0, 1), (6000, 0, 0), (6000, 6000, 0)], format="xyb", close=True
    )
    d.saveas(p)
    with pytest.raises(ValueError, match="bulges"):
        inspect(p, "um", tmp_path / "out")


def test_undecodable_pattern_is_not_registered(tmp_path):
    source = drawing(tmp_path / "device.dxf")
    out = tmp_path / "out"
    i = inspect(source, "um", out)
    with pytest.raises(ValueError, match="discrimination failed"):
        generate(
            out / "normalized.dxf",
            {
                "inspection": i,
                "id": "rejected-pattern",
                "name": "Test",
                "seed": 8,
                "designs": [],
            },
            out,
        )
    assert not (out / "registry.json").exists()
    assert not (out / "mib-map.zip").exists()


def test_real_dwg_conversion(tmp_path):
    import shutil
    dwg = Path("/opt/cad-test-fixtures/Line.dwg")
    if not shutil.which("dwg2dxf") or not dwg.exists():
        pytest.skip("Run this test inside the LibreDWG-enabled image")
    out = tmp_path / "dwg-out"
    i = inspect(dwg, "mm", out)
    assert i["no_dot_regions"] == 0
    assert i["open_fragments"] > 0


def test_explicit_regions_exclude_interior_whole_dot_and_clearance():
    import numpy as np
    from regions import regions, outside_regions
    d = ezdxf.new()
    d.layers.new('NO_DOT')
    d.modelspace().add_lwpolyline([(0,0),(1000,0),(1000,300),(300,300),(300,1000),(0,1000)],close=True,dxfattribs={'layer':'NO_DOT'})
    d.modelspace().add_circle((2000,2000),100,dxfattribs={'layer':'NO_DOT'})
    pts = np.array([[100,500],[500,500],[500,350],[500,356],[500,357],[2000,2000],[2156,2000],[2157,2000]])
    assert outside_regions(pts,regions(d),56).tolist() == [False,True,False,False,True,False,False,True]


def test_ambiguous_and_self_crossing_regions_fail_closed():
    from regions import regions
    d = ezdxf.new()
    with pytest.raises(ValueError, match='NO_DOT'):
        regions(d)
    d.layers.new('NO_DOT')
    e = d.modelspace().add_lwpolyline([(0,0),(100,100),(0,100),(100,0)], dxfattribs={'layer':'NO_DOT'})
    with pytest.raises(ValueError, match='open boundaries'):
        regions(d)
    e.closed = True
    with pytest.raises(ValueError, match='self-intersect'):
        regions(d)


def test_empty_region_payload_rejected_before_preview(tmp_path):
    p = drawing(tmp_path / "empty-region.dxf")
    d = ezdxf.readfile(p)
    d.modelspace().add_region()
    # ezdxf itself drops an empty REGION on write; exercise the imported
    # entity directly (LibreDWG emits these empty records in real output).
    from cad_import import flatten_regions
    with pytest.raises(ValueError, match="CAD_GEOMETRY_LOSS"):
        flatten_regions(d)



def test_planar_region_preserved_as_closed_polygon(tmp_path):
    from ezdxf.acis import api as acis
    from ezdxf.render import MeshBuilder
    p = drawing(tmp_path / "region.dxf")
    d = ezdxf.readfile(p)
    mesh = MeshBuilder()
    mesh.add_face([(200,200,0),(400,200,0),(400,400,0),(200,400,0)])
    acis.export_dxf(d.modelspace().add_region(), [acis.body_from_mesh(mesh)])
    d.saveas(p)
    i = inspect(p, "um", tmp_path / "out")
    assert i["converted_regions"] == 1
    result = ezdxf.readfile(tmp_path / "out/normalized.dxf")
    assert len(result.modelspace().query("REGION")) == 0
    assert len(result.modelspace().query("LWPOLYLINE")) == 3


def test_unlabelled_closed_channel_selection_roundtrip(tmp_path):
    import numpy as np
    p = drawing(tmp_path / "plain.dxf")
    d = ezdxf.readfile(p)
    for e in d.modelspace(): e.dxf.layer = "0"
    d.saveas(p)
    out = tmp_path / "out"
    i = inspect(p, "um", out)
    assert i["no_dot_regions"] == 0
    meta = {"inspection":i,"id":"selected-channel","name":"Selected channel","seed":9,"designs":[]}
    with pytest.raises(ValueError,match="NO_DOT"):
        generate(out / "normalized.dxf",meta,out)
    # The central LINE splits the channel into two closed faces: select both.
    i["selected_regions"] = [r["id"] for r in i["region_candidates"] if r["kind"]=="polygon" and all(1000<=p[0]<=5000 and 2900<=p[1]<=3100 for p in r["points"])]
    assert len(i["selected_regions"])==2
    generate(out / "normalized.dxf",meta,out)
    result=ezdxf.readfile(out / "design_with_dotgrid.dxf")
    assert len(result.modelspace().query('LWPOLYLINE')) == 2  # no review-only polygons exported
    # Independent rectangle-vs-circle clearance calculation on exported dots.
    for e in result.modelspace().query('CIRCLE[layer=="DOTGRID"]'):
        x,y=e.dxf.center.x,e.dxf.center.y
        distance=np.hypot(max(1000-x,0,x-5000), max(2900-y,0,y-3100))
        assert distance > 56


def test_nonplanar_region_is_not_partially_imported(tmp_path):
    from ezdxf.acis import api as acis
    from ezdxf.render.forms import cube
    p = drawing(tmp_path / "solid-region.dxf")
    d=ezdxf.readfile(p)
    acis.export_dxf(d.modelspace().add_region(),[acis.body_from_mesh(cube())])
    d.saveas(p)
    with pytest.raises(ValueError,match="CAD_REGION_UNSUPPORTED"):
        inspect(p,"um",tmp_path / "out")


def test_actual_wafer_opt_in(tmp_path):
    import os
    source = os.environ.get("CAD_WAFER_FIXTURE")
    if not source or not os.environ.get("CAD_ODA_EXECUTABLE"):
        pytest.skip("Private wafer source and preservation converter required")
    i=inspect(Path(source),"um",tmp_path / "wafer")
    assert i["converted_regions"]==88
    assert i["extent_um"]==[110000,110000]
    assert i["open_fragments"]==20
    assert len(i["region_candidates"])==3210
    assert i["generation_supported"] is True
    assert any("header says mm" in w for w in i["warnings"])
