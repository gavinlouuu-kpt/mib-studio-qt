"""Closed-area candidates for explicit review; topology is not fluid semantics."""
import math
from ezdxf.path import make_path
from shapely.geometry import LineString, Polygon
from shapely.affinity import translate
from collections import defaultdict
from shapely.ops import polygonize_full, unary_union


def candidates(doc):
    lines = []; circles = []; vertices = 0; outlines = defaultdict(list)
    for e in doc.modelspace():
        if e.dxf.layer.upper() == 'NO_DOT':
            continue
        typ = e.dxftype()
        if typ == 'CIRCLE':
            # Large circles are also selectable; none is silently called a port.
            circles.append({'kind':'circle','center':[e.dxf.center.x,e.dxf.center.y], 'radius':e.dxf.radius, 'layers':[e.dxf.layer]})
        elif typ in {'LINE','ARC','LWPOLYLINE'}:
            p = [(round(v.x,6),round(v.y,6)) for v in make_path(e).flattening(.05)]
            vertices += len(p)
            if vertices > 200000:
                raise ValueError('CAD_TOPOLOGY_LIMIT: Too many boundary vertices')
            if len(p)>1: lines.append(LineString(p))
            if typ=='LWPOLYLINE' and e.closed:
                shape=Polygon(p)
                outlines[tuple(round(v,3) for v in shape.bounds)].append((shape,e.dxf.layer))
    polygons, cuts, dangles, invalid = polygonize_full(unary_union(lines))
    result = circles
    for polygon in polygons.geoms:
        # Preserve face topology. A cut outline uses the exterior; a fluid face retains holes.
        p = list(polygon.exterior.coords)[:-1]
        if len(p)>20000:
            raise ValueError('CAD_TOPOLOGY_LIMIT: A boundary exceeds 20000 vertices')
        layers=[layer for outline,layer in outlines[tuple(round(v,3) for v in polygon.bounds)] if Polygon(p).equals_exact(outline,1e-5,normalize=True)]
        result.append({'layers': sorted(set(layers)), 'kind':'polygon','points':[[x,y] for x,y in p], 'holes': [[[x,y] for x,y in list(r.coords)[:-1]] for r in polygon.interiors]})
    if len(result)>5000:
        raise ValueError('CAD_TOPOLOGY_LIMIT: More than 5000 candidate areas')
    for n,r in enumerate(result): r['id']=n
    open_paths=[list(g.coords) for collection in (cuts,dangles,invalid) for g in collection.geoms]
    return result, open_paths


def matching_groups(choices):
    """Translated copies only, with matching vertex topology including holes.

    No semantic classification or scaling/reflection/rotation inference.
    """
    buckets=defaultdict(list)
    for c in choices:
        if c['kind']!='polygon': continue
        p=Polygon(c['points'],c.get('holes',[]))
        # Bound comparisons; small marks do not need wafer-wide propagation.
        if p.area<1e6:continue
        x0,y0,x1,y1=p.bounds
        key=(round(x1-x0,2),round(y1-y0,2),len(p.exterior.coords),len(p.interiors))
        buckets[key].append((c['id'],translate(p,-x0,-y0)))
    groups=[]
    for items in buckets.values():
        bins=[]
        for n,p in items:
            for representative,ids in bins:
                if p.equals_exact(representative,.01,normalize=True):
                    ids.append(n);break
            else: bins.append((p,[n]))
        groups.extend(ids for p,ids in bins if len(ids)>1)
    return groups
