import sys
from pathlib import Path
import numpy as np
import pytest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'generator'))
from placement import Placement, selected_areas
from batched import RegionFilter


def fixture():
    return {'placement_contract':1,'extent_um':[1000,1000],'chip_regions':[0],'selected_regions':[1], 'cut_clearance_um':10,
        'region_candidates':[
          {'kind':'polygon','points':[[0,0],[1000,0],[1000,1000],[0,1000]]},
          {'kind':'polygon','points':[[100,100],[900,100],[900,900],[100,900]],'holes':[[[300,300],[700,300],[700,700],[300,700]]]},
        ]}


def test_cut_boundary_and_fluid_holes_are_independent():
    i=fixture();p=Placement(i);f=RegionFilter(selected_areas(i),56.1)
    xy=np.array([[500,500],[150,150],[15,20],[20,20],[340,500],[370,500],[-10,500],[1010,500]])
    assert (p.keep(xy)&f.keep(xy)).tolist()==[True,False,False,True,False,True,False,False]
    assert p.counts(np.column_stack((np.zeros((len(xy),2)),xy)))==[6]


def test_missing_cut_boundary_is_not_replaced_by_drawing_bounds():
    i=fixture();i['chip_regions']=[]
    with pytest.raises(ValueError,match='final chip boundaries'):Placement(i)
    i['manual_chip_rectangles']=[[0,0,1000,1000]]
    assert len(Placement(i).polys)==1


def test_conflicting_nested_out_of_bounds_and_degenerate_boundaries_rejected():
    i=fixture();i['chip_regions']=[1]
    with pytest.raises(ValueError,match='distinct'):Placement(i)
    i=fixture();i['manual_chip_rectangles']=[[10,10,990,990]]
    with pytest.raises(ValueError,match='overlap'):Placement(i)
    i=fixture();i['manual_chip_rectangles']=[[-150001,0,1000,1000]]
    with pytest.raises(ValueError,match='supported range'):Placement(i)
    i=fixture();i['chip_regions']=[];i['manual_chip_rectangles']=[[0,0,10,10]]
    with pytest.raises(ValueError,match='usable interior'):Placement(i)
    i=fixture();i['cut_clearance_um']=float('nan')
    with pytest.raises(ValueError,match='clearance'):Placement(i)


def test_translated_copy_matching_preserves_holes_and_rejects_changed_topology():
    from domains import matching_groups
    c={'id':0,'kind':'polygon','points':[[0,0],[3000,0],[3000,3000],[0,3000]],'holes':[[[1000,1000],[2000,1000],[2000,2000],[1000,2000]]]}
    copy={'id':1,'kind':'polygon','points':[[x+4000,y+500] for x,y in c['points']], 'holes':[[[x+4000,y+500] for x,y in hole] for hole in c['holes']]}
    different={**copy,'id':2,'holes':[]}
    assert matching_groups([c,copy,different])==[[0,1]]


def test_import_candidates_preserve_holes_and_source_layer_hints():
    import ezdxf
    from domains import candidates
    d=ezdxf.new();d.layers.new('CHIP_BOUNDARY')
    d.modelspace().add_lwpolyline([(0,0),(3000,0),(3000,3000),(0,3000)],close=True,dxfattribs={'layer':'CHIP_BOUNDARY'})
    d.modelspace().add_lwpolyline([(1000,1000),(2000,1000),(2000,2000),(1000,2000)],close=True)
    choices,open_paths=candidates(d)
    outer=next(c for c in choices if c.get('holes'))
    assert len(outer['holes'])==1
    assert outer['layers']==['CHIP_BOUNDARY']
    assert open_paths==[]
