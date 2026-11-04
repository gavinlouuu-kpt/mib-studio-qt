"""Bounded-memory adapter for the frozen codec and CAD exports."""
from pathlib import Path
import io
import numpy as np
from scipy.spatial import cKDTree
import gdstk
from shapely import STRtree, points as shape_points
from shapely.geometry import Polygon

BATCH = 65536
MAX_NODES = 14_000_000
MAX_AXIS = 3700


def supported(columns, rows):
    return max(columns, rows) <= MAX_AXIS and columns * rows <= MAX_NODES


class RegionFilter:
    def __init__(self, areas, clearance):
        self.clearance = clearance
        self.circles = [a for k, a in areas if k == 'circle']
        polygons = [Polygon(a) for k, a in areas if k == 'polygon']
        self.tree = STRtree(polygons) if polygons else None

    def keep(self, xy):
        keep = np.ones(len(xy), dtype=bool)
        if self.tree is not None and len(xy):
            hits = self.tree.query(shape_points(xy), predicate='dwithin', distance=self.clearance)
            keep[hits[0]] = False
        # Exact circle distance, bounded to candidates in its expanded bounding box.
        for x, y, r in self.circles:
            radius = r + self.clearance
            ids = np.flatnonzero(keep & (np.abs(xy[:, 0]-x) <= radius) & (np.abs(xy[:, 1]-y) <= radius))
            keep[ids] = np.hypot(xy[ids, 0]-x, xy[ids, 1]-y) > radius
        return keep


def pattern_batches(cb, geometry_pts, chips, marks, wafer, keepout, areas, extent, clearance):
    """Same ordered nodes and displacement bits as core build_pattern; no global mesh."""
    tree = cKDTree(geometry_pts) if len(geometry_pts) else None
    mark_tree = cKDTree(np.vstack(marks)) if marks else None
    filters = RegionFilter(areas, clearance)
    mns, phi, psi = map(np.asarray, (cb.mns, cb.phi, cb.psi))
    w, h = extent
    radius = cb.dot_diameter_um / 2
    for start in range(0, cb.columns * cb.rows, BATCH):
        flat = np.arange(start, min(start+BATCH, cb.columns*cb.rows))
        ii, jj = flat // cb.rows, flat % cb.rows
        x, y = cb.origin_um[0]+ii*cb.pitch_um, cb.origin_um[1]+jj*cb.pitch_um
        keep = np.ones(len(ii), bool)
        if wafer is not None:
            cx, cy, r = wafer
            keep &= np.hypot(x-cx, y-cy) < r-keepout.wafer_edge_um
        if tree is not None:
            d, _ = tree.query(np.column_stack((x[keep],y[keep])), distance_upper_bound=keepout.channel_um+cb.displacement_um+cb.dot_diameter_um)
            keep[keep] = d > keepout.channel_um+cb.displacement_um+radius
        for x0,y0,x1,y1 in chips:
            m = keepout.dicing_um
            near = ((np.abs(x-x0)<m)|(np.abs(x-x1)<m)) & (y>y0-m) & (y<y1+m)
            near |= ((np.abs(y-y0)<m)|(np.abs(y-y1)<m)) & (x>x0-m) & (x<x1+m)
            keep &= ~near
        if mark_tree is not None:
            d, _ = mark_tree.query(np.column_stack((x,y)), distance_upper_bound=keepout.existing_marks_um)
            keep &= d > keepout.existing_marks_um
        ii,jj,x,y = ii[keep],jj[keep],x[keep],y[keep]
        bx,by = mns[(jj+phi[ii])%63],mns[(ii+psi[jj])%63]
        x = x + np.where(by==0, 1-2*bx, 0)*cb.displacement_um
        y = y + np.where(by==1, 1-2*bx, 0)*cb.displacement_um
        xy = np.column_stack((x,y))
        keep = filters.keep(xy) & (x>=radius) & (x<=w-radius) & (y>=radius) & (y<=h-radius)
        yield np.column_stack((ii[keep],jj[keep],x[keep],y[keep]))


def spool_pattern(path, batches):
    count = 0
    with open(path, 'wb') as stream:
        for dots in batches:
            dots.astype(np.float64, copy=False).tofile(stream)
            count += len(dots)
    return np.memmap(path, dtype=np.float64, mode='r', shape=(count,4)) if count else np.empty((0,4))


def write_dxf_stream(doc, dots, cb, path):
    """Preserve all source sections; stream circles with reserved unique handles."""
    if 'DOTGRID' not in doc.layers:
        doc.layers.add('DOTGRID', color=1)
    # First serialization creates any late DXF bookkeeping resources.
    doc.write(io.StringIO())
    first = int(str(doc.entitydb.handles), 16)
    doc.entitydb.handles.reset(format(first+len(dots), 'X'))
    text = io.StringIO()
    doc.write(text)
    lines = text.getvalue().splitlines(keepends=True)
    in_entities = False
    split = None
    for k in range(0, len(lines)-1, 2):
        code, value = lines[k].strip(), lines[k+1].strip()
        if code == '2' and value == 'ENTITIES':
            in_entities = True
        elif in_entities and code == '0' and value == 'ENDSEC':
            split = k
            break
    if split is None:
        raise ValueError('DXF export has no ENTITIES section')
    owner = doc.modelspace().block_record_handle
    # R2007+ is UTF-8; older normalized drawings retain their declared codepage.
    encoding = 'utf-8' if doc.dxfversion >= 'AC1021' else doc.encoding
    with open(path, 'w', encoding=encoding, errors='dxfreplace') as f:
        f.writelines(lines[:split])
        for start in range(0,len(dots),BATCH):
            f.writelines(
                f'  0\nCIRCLE\n  5\n{first+start+n:X}\n330\n{owner}\n100\nAcDbEntity\n  8\nDOTGRID\n100\nAcDbCircle\n 10\n{x:.9f}\n 20\n{y:.9f}\n 30\n0.0\n 40\n{cb.dot_diameter_um/2:.9f}\n'
                for n,(_,_,x,y) in enumerate(dots[start:start+BATCH])
            )
        f.writelines(lines[split:])


def write_gds_stream(dots, cb, path):
    writer = gdstk.GdsWriter(str(path), unit=1e-6, precision=1e-9)
    try:
        dot = gdstk.Cell('DOT')
        dot.add(gdstk.ellipse((0,0), cb.dot_diameter_um/2, layer=10, tolerance=.05))
        writer.write(dot)
        top = gdstk.Cell('DOTGRID')
        for start in range(0,len(dots),BATCH):
            cell = top if len(dots)<=BATCH else gdstk.Cell(f'B{start//BATCH}')
            for _,_,x,y in dots[start:start+BATCH]:
                cell.add(gdstk.Reference('DOT', (float(x),float(y))))
            if cell is not top:
                writer.write(cell)
                top.add(gdstk.Reference(cell.name))
        writer.write(top)
    finally:
        writer.close()
    return True


def dot_preview(dots, extent):
    """Overview uses every dot; magnified detail contains exact centres."""
    import base64
    import cv2
    size = 1600
    w,h = extent
    raster = np.zeros((size,size,4), dtype=np.uint8)
    cx,cy = dots[len(dots)//2,2:4]
    bounds = [max(0,float(cx)-500),max(0,float(cy)-500),min(w,float(cx)+500),min(h,float(cy)+500)]
    detail = []
    for start in range(0,len(dots),BATCH):
        xy=dots[start:start+BATCH,2:4]
        px=np.clip((xy[:,0]/w*(size-1)).astype(int),0,size-1)
        py=np.clip(((1-xy[:,1]/h)*(size-1)).astype(int),0,size-1)
        raster[py,px]=(180,90,20,190)
        keep=(xy[:,0]>=bounds[0])&(xy[:,0]<=bounds[2])&(xy[:,1]>=bounds[1])&(xy[:,1]<=bounds[3])
        detail.extend(xy[keep].tolist())
    ok, encoded = cv2.imencode('.png',raster)
    if not ok: raise ValueError('Dot preview failed')
    return {'dot_count':len(dots),'overview_png':base64.b64encode(encoded).decode('ascii'),'detail_bounds':bounds,'detail_points':detail,'dot_radius_um':6}
