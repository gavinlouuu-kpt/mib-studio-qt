"""Independent exported-geometry check using GDS boolean offsets (not generator filter)."""
import sys
import json
from pathlib import Path
import ezdxf
import gdstk
import numpy as np

dxf = ezdxf.readfile(sys.argv[1])
areas = [gdstk.Polygon(e.get_points('xy')) for e in dxf.modelspace() if e.dxf.layer == 'NO_DOT' and e.dxftype() == 'LWPOLYLINE']
assert areas, 'Missing explicit fluidic domain'
dots = [e for e in dxf.modelspace() if e.dxf.layer == 'DOTGRID']
assert dots and all(e.dxftype() == 'CIRCLE' for e in dots)
radius = max(e.dxf.radius for e in dots)
# Rounded offset is conservatively expanded 0.1 um to cover polygon approximation.
protected = gdstk.offset(areas, 50 + radius + .1, join='round', tolerance=256, precision=.0001)
xy = np.array([(e.dxf.center.x,e.dxf.center.y) for e in dots])
violations = sum(gdstk.inside(xy, protected))
gds = gdstk.read_gds(sys.argv[2])
gxy = np.array([ref.origin for ref in gds['DOTGRID'].references])
assert len(gxy) == len(xy) and np.allclose(gxy,xy,atol=.001)
assert all(np.linalg.norm(p.points,axis=1).max() <= radius+.001 for p in gds['DOT'].polygons)
assert violations == 0, f'{violations} dots intersect fluidic domain or edge clearance'
assert not any(gdstk.inside(gxy, protected))
print(json.dumps({'dot_count':len(dots),'channel_port_or_clearance_violations':violations,'edge_clearance_um':50,'dxf_gds_positions_match':True}))
