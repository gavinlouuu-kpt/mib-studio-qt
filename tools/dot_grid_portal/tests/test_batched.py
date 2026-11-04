"""Full-size adapter equivalence, round trips, and boundary regression tests."""
import sys
from pathlib import Path
import numpy as np
import ezdxf
import gdstk
import pytest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'generator'))
import pipeline
from batched import pattern_batches, RegionFilter, write_dxf_stream, write_gds_stream, supported
from regions import outside_regions
from dotgrid.codebook import generate_codebook
from dotgrid.pattern import build_pattern, KeepOut


def test_batched_matches_frozen_scalar_engine(monkeypatch):
    import batched
    monkeypatch.setattr(batched,'BATCH',73)
    cb=generate_codebook(9,60,55,pitch_um=30,dot_diameter_um=12,displacement_um=5)
    pts=np.array([[300.,200.],[900.,800.]])
    chips=[(100.,100.,1500.,1400.)]
    marks=[np.array([[400.,400.],[420.,430.]])]
    wafer=(850.,850.,1000.)
    ko=KeepOut(50,20,30,100)
    areas=[('circle',(700,700,100)),('polygon',np.array([[1000,1000],[1300,1000],[1200,1300]]))]
    expected=build_pattern(cb,pts,chips,marks,wafer,ko)
    expected=expected[outside_regions(expected[:,2:],areas,56)]
    expected=expected[(expected[:,2]>=6)&(expected[:,2]<=1794)&(expected[:,3]>=6)&(expected[:,3]<=1644)]
    actual=np.concatenate(list(pattern_batches(cb,pts,chips,marks,wafer,ko,areas,(1800,1650),56)))
    np.testing.assert_array_equal(actual,expected)


def test_indexed_regions_match_exact_reference():
    areas=[('circle',(40,20,15)),('polygon',np.array([[0,0],[10,0],[10,20],[0,20]]))]
    xy=np.concatenate([np.random.default_rng(9).uniform(-100,150,(10000,2)), [[66,10],[66.0001,10],[-56,10],[-56.0001,10],[0,0]]])
    np.testing.assert_array_equal(RegionFilter(areas,56).keep(xy),outside_regions(xy,areas,56))


def test_streamed_exports_preserve_device_and_unique_handles(tmp_path,monkeypatch):
    import batched
    monkeypatch.setattr(batched,'BATCH',11)
    cb=generate_codebook(9,30,30)
    dots=np.array([(i,j,*cb.dot_um(i,j)) for i in range(30) for j in range(30)])
    d=ezdxf.new('R2000');d.units=13
    d.modelspace().add_line((0,0),(100,200))
    d.modelspace().add_text('device annotation')
    write_dxf_stream(d,dots,cb,tmp_path/'out.dxf')
    result=ezdxf.readfile(tmp_path/'out.dxf')
    assert not result.audit().has_errors
    assert len(result.modelspace().query('LINE'))==1
    assert len(result.modelspace().query('TEXT'))==1
    circles=list(result.modelspace().query('CIRCLE'))
    assert len(circles)==len(dots)
    assert len({e.dxf.handle for e in result.entitydb.values()})==len(result.entitydb)
    np.testing.assert_array_equal([(e.dxf.center.x,e.dxf.center.y) for e in circles],dots[:,2:])
    write_gds_stream(dots,cb,tmp_path/'out.gds')
    lib=gdstk.read_gds(tmp_path/'out.gds')
    assert len(lib.top_level())==1 and lib.top_level()[0].name=='DOTGRID'
    assert sum(len(c.references) for c in lib.cells if c.name.startswith('B'))==len(dots)
    assert len(lib['DOTGRID'].get_polygons())==len(dots)
    assert supported(3668,3668)
    assert not supported(5001,5001)


def test_compact_oasis_roundtrip_and_converter_failure(tmp_path, monkeypatch):
    from compact import compact_exports
    import klayout.db as kdb
    monkeypatch.delenv('CAD_ODA_EXECUTABLE', raising=False)
    cb=generate_codebook(9,30,30)
    dots=np.array([(i,j,*cb.dot_um(i,j)) for i in range(30) for j in range(30)])
    write_gds_stream(dots,cb,tmp_path/'dotgrid_layer.gds')
    assert compact_exports(tmp_path)==['dotgrid_layer.oas']
    layout=kdb.Layout(); layout.read(str(tmp_path/'dotgrid_layer.oas'))
    iterator=layout.top_cell().begin_shapes_rec(layout.layer(10,0)); xy=[]
    while not iterator.at_end():
        xy.append((iterator.trans().disp.x,iterator.trans().disp.y));iterator.next()
    assert sorted(xy)==sorted(map(tuple,np.rint(dots[:,2:]*1000).astype(int)))
    assert (tmp_path/'dotgrid_layer.oas').stat().st_size < (tmp_path/'dotgrid_layer.gds').stat().st_size/4
    (tmp_path/'design_with_dotgrid.dxf').write_text('test',encoding='utf-8')
    monkeypatch.setenv('CAD_ODA_EXECUTABLE','/bin/false')
    with pytest.raises(ValueError,match='CAD_EXPORT_FAILED'):compact_exports(tmp_path)
