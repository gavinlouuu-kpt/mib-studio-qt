"""Original illustrative microfluidic CAD, not a validated device or customer file."""
from pathlib import Path
import ezdxf
out=Path(__file__).resolve().parents[1]/'web/public/demo/sorter-concept.dxf'
d=ezdxf.new('R2010');d.units=4
for name in ['CHIP_OUTLINE','CHANNEL_WALLS','PORTS','ALIGNMENT']:
    d.layers.new(name)
m=d.modelspace()
m.add_lwpolyline([(0,0),(12,0),(12,8),(0,8)],close=True,dxfattribs={'layer':'CHIP_OUTLINE'})
def line(a,b,layer='CHANNEL_WALLS'):m.add_line(a,b,dxfattribs={'layer':layer})
def path(points):
    for a,b in zip(points,points[1:]):line(a,b)
# Two inlet branches join a serpentine mixer, then an observation region and two outlets.
for y in [2,6]:
    m.add_circle((1.2,y),.45,dxfattribs={'layer':'PORTS'})
path([(1.65,1.9),(2.7,1.9),(2.7,3.9),(3.5,3.9),(3.5,2.9),(4.5,2.9),(4.5,4.9),(5.5,4.9),(5.5,2.9),(6.5,2.9),(6.5,3.65),(8.5,3.65),(9.2,2.9),(10.35,2.9)])
path([(1.65,2.1),(2.5,2.1),(2.5,4.1),(3.7,4.1),(3.7,3.1),(4.3,3.1),(4.3,5.1),(5.7,5.1),(5.7,3.1),(6.3,3.1),(6.3,4.35),(8.5,4.35),(9.2,5.1),(10.35,5.1)])
path([(1.65,5.9),(2.5,5.9),(2.5,4.1)])
path([(1.65,6.1),(2.7,6.1),(2.7,4.3),(3.3,4.3)])
path([(8.8,4),(9.3,3.1),(10.35,3.1)])
path([(8.8,4),(9.3,4.9),(10.35,4.9)])
for y in [3,5]:m.add_circle((10.8,y),.45,dxfattribs={'layer':'PORTS'})
for x,y in [(0.6,.6),(11.4,.6),(.6,7.4),(11.4,7.4)]:
    line((x-.15,y),(x+.15,y),'ALIGNMENT');line((x,y-.15),(x,y+.15),'ALIGNMENT')
d.saveas(out)
print(out)
