"""Closed-area candidates for explicit review; topology is not fluid semantics."""
import math
from ezdxf.path import make_path
from shapely.geometry import LineString
from shapely.ops import polygonize_full, unary_union


def candidates(doc):
    lines = []; circles = []; vertices = 0
    for e in doc.modelspace():
        if e.dxf.layer.upper() == 'NO_DOT':
            continue
        typ = e.dxftype()
        if typ == 'CIRCLE':
            # Large circles are also selectable; none is silently called a port.
            circles.append({'kind':'circle','center':[e.dxf.center.x,e.dxf.center.y], 'radius':e.dxf.radius})
        elif typ in {'LINE','ARC','LWPOLYLINE'}:
            p = [(round(v.x,6),round(v.y,6)) for v in make_path(e).flattening(.05)]
            vertices += len(p)
            if vertices > 200000:
                raise ValueError('CAD_TOPOLOGY_LIMIT: Too many boundary vertices')
            if len(p)>1: lines.append(LineString(p))
    polygons, cuts, dangles, invalid = polygonize_full(unary_union(lines))
    result = circles
    for polygon in polygons.geoms:
        # Filling holes is conservative; it never permits dots in an excluded area.
        p = list(polygon.exterior.coords)[:-1]
        if len(p)>20000:
            raise ValueError('CAD_TOPOLOGY_LIMIT: A boundary exceeds 20000 vertices')
        result.append({'kind':'polygon','points':[[x,y] for x,y in p]})
    if len(result)>5000:
        raise ValueError('CAD_TOPOLOGY_LIMIT: More than 5000 candidate areas')
    for n,r in enumerate(result): r['id']=n
    return result, len(cuts.geoms)+len(dangles.geoms)+len(invalid.geoms)


def apply_selection(doc, inspection):
    choices = inspection.get('region_candidates', [])
    selection = inspection.get('selected_regions', [])
    if not isinstance(selection,list) or any(type(x) is not int or x<0 or x>=len(choices) for x in selection):
        raise ValueError('Invalid no-dot selection')
    if 'NO_DOT' not in doc.layers: doc.layers.new('NO_DOT')
    added = []
    for n in set(selection):
        r=choices[n]
        if r['kind']=='circle':
            added.append(doc.modelspace().add_circle(r['center'],r['radius'],dxfattribs={'layer':'NO_DOT'}))
        else:
            added.append(doc.modelspace().add_lwpolyline(r['points'],close=True,dxfattribs={'layer':'NO_DOT'}))
    return added
