"""Explicit filled no-dot regions. Never infer fluid interiors from open walls."""
import numpy as np
import gdstk
from shapely.geometry import Polygon


def regions(doc):
    result = []
    for e in doc.modelspace():
        if e.dxf.layer.upper() != 'NO_DOT':
            continue
        if e.dxftype() == 'CIRCLE' and e.dxf.radius > 0:
            result.append(('circle', (e.dxf.center.x, e.dxf.center.y, e.dxf.radius)))
        elif e.dxftype() == 'LWPOLYLINE' and e.closed and not e.has_arc:
            p = np.asarray(e.get_points('xy'), dtype=float)
            if len(p) > 20000 or len(p) < 3 or not np.isfinite(p).all():
                raise ValueError('No-dot polygons require 3–20000 finite vertices')
            if np.array_equal(p[0], p[-1]):
                p = p[:-1]
            if (np.linalg.norm(np.roll(p,-1,axis=0)-p,axis=1)<1e-9).any():
                raise ValueError('No-dot polygon has duplicate vertices')
            poly = Polygon(p)
            if not poly.is_valid:
                raise ValueError('No-dot polygon must not self-intersect')
            if poly.area < 1e-9:
                raise ValueError('No-dot polygon has no area')
            result.append(('polygon', p))
        else:
            raise ValueError('NO_DOT must contain closed straight polylines or circles; open boundaries are ambiguous')
    if not result:
        raise ValueError('Define closed channel, chamber and port areas on a NO_DOT layer before generation; open walls do not define safe interiors')
    return result


def outside_regions(points, areas, clearance_um):
    """Keep centres strictly outside filled areas plus radius and edge clearance."""
    points = np.asarray(points, dtype=float)
    keep = np.ones(len(points), dtype=bool)
    for kind, area in areas:
        if kind == 'circle':
            x, y, r = area
            keep &= np.linalg.norm(points - [x,y], axis=1) > r + clearance_um
        else:
            p = area
            keep &= ~np.asarray(gdstk.inside(points, [gdstk.Polygon(p)]))
            for a,b in zip(p, np.roll(p,-1,axis=0)):
                v = b-a
                t = np.clip(((points-a)*v).sum(axis=1)/(v*v).sum(), 0, 1)
                keep &= np.linalg.norm(points-(a+t[:,None]*v), axis=1) > clearance_um
    return keep
