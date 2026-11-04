"""Explicit post-cut domains, distinct from filled fluid faces (including holes)."""
import math
import numpy as np
from shapely import STRtree, points
from shapely.geometry import Polygon, Point


def face(candidate, outline=False):
    if candidate['kind']=='circle':
        x,y=candidate['center'];r=candidate['radius']
        if not all(math.isfinite(v) for v in (x,y,r)) or r<=0:
            raise ValueError('Invalid circle')
        return Point(x,y).buffer(r, quad_segs=256)
    p=Polygon(candidate['points'], [] if outline else candidate.get('holes', []))
    if not p.is_valid or p.is_empty or p.area<=0:
        raise ValueError('Invalid closed region')
    return p


def selected_areas(inspection):
    choices=inspection.get('region_candidates',[])
    ids=inspection.get('selected_regions',[])
    if not isinstance(ids,list) or len(ids)>5000 or any(type(n) is not int or not 0<=n<len(choices) for n in ids):
        raise ValueError('Invalid no-dot selection')
    return [('circle',(*choices[n]['center'],choices[n]['radius'])) if choices[n]['kind']=='circle' else ('polygon',face(choices[n])) for n in set(ids)]


class Placement:
    def __init__(self, inspection, radius=6):
        if inspection.get('placement_contract')!=1:
            raise ValueError('Confirm post-cut chip boundaries before generation')
        choices=inspection.get('region_candidates',[])
        ids=inspection.get('chip_regions',[])
        rectangles=inspection.get('manual_chip_rectangles',[])
        if not isinstance(ids,list) or len(ids)>100 or any(type(n) is not int or not 0<=n<len(choices) for n in ids):
            raise ValueError('Invalid chip boundary selection')
        if len(set(ids))!=len(ids) or set(ids)&set(inspection.get('selected_regions',[])):
            raise ValueError('Chip boundaries and fluid regions must be distinct')
        if not isinstance(rectangles,list) or len(ids)+len(rectangles)>100:
            raise ValueError('At most 100 chip boundaries are supported')
        self.shapes=[choices[n] for n in ids]
        w,h=inspection['extent_um']
        for rect in rectangles:
            if not isinstance(rect,list) or len(rect)!=4 or any(type(v) not in (int,float) or not math.isfinite(v) for v in rect):
                raise ValueError('Invalid manual chip rectangle')
            x0,y0,x1,y1=rect
            if not (-150000<=x0<x1<=150000 and -150000<=y0<y1<=150000):
                raise ValueError('Manual chip boundary coordinates exceed the supported range')
            self.shapes.append({'kind':'polygon','points':[[x0,y0],[x1,y0],[x1,y1],[x0,y1]]})
        self.bounds=(min([0]+[r[0] for r in rectangles]),min([0]+[r[1] for r in rectangles]),max([w]+[r[2] for r in rectangles]),max([h]+[r[3] for r in rectangles]))
        if not self.shapes:
            raise ValueError('Select or define the final chip boundaries; drawing bounds are not chip boundaries')
        clearance=inspection.get('cut_clearance_um',200)
        if type(clearance) not in (int,float) or not math.isfinite(clearance) or not 0<=clearance<=5000:
            raise ValueError('Cut clearance must be 0–5000 micrometres')
        self.clearance=clearance+radius+0.1
        self.polys=[face(c,outline=True) for c in self.shapes]
        tree=STRtree(self.polys)
        for n,p in enumerate(self.polys):
            if p.buffer(-self.clearance).is_empty:
                raise ValueError('Chip has no usable interior after cutting clearance')
            for k in tree.query(p,predicate='intersects'):
                if k>n and p.intersection(self.polys[k]).area>1e-6:
                    raise ValueError('Chip boundaries overlap or nest; choose one final edge per chip')
        self.tree=tree
        self.edges=STRtree([p.boundary for p in self.polys])

    def keep(self, xy):
        queries=points(xy)
        keep=np.zeros(len(xy),bool)
        if not len(xy):return keep
        hits=self.tree.query(queries,predicate='within')
        keep[hits[0]]=True
        near=self.edges.query(queries,predicate='dwithin',distance=self.clearance)
        keep[near[0]]=False
        # Polygonized circles are inscribed; exact distance retains a conservative
        # result and avoids admitting dots outside the true radius clearance.
        for c in self.shapes:
            if c['kind']=='circle':
                x,y=c['center'];r=c['radius']
                inside=np.hypot(xy[:,0]-x,xy[:,1]-y)<r
                keep[inside]&=np.hypot(xy[inside,0]-x,xy[inside,1]-y)<r-self.clearance
        return keep

    def counts(self, dots, batch=65536):
        counts=np.zeros(len(self.polys),dtype=np.int64)
        for start in range(0,len(dots),batch):
            hits=self.tree.query(points(dots[start:start+batch,2:4]),predicate='within')
            counts+=np.bincount(hits[1],minlength=len(counts))
        return counts.tolist()
