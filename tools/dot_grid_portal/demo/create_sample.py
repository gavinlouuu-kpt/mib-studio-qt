"""Original connected fluidic concept; explicit filled channel/port no-dot domain."""
from pathlib import Path
import ezdxf
import gdstk

out = Path(__file__).resolve().parents[1] / 'web/public/demo/sorter-concept.dxf'
d = ezdxf.new('R2010')
d.units = 4
for name in ['CHIP_OUTLINE', 'NO_DOT', 'ALIGNMENT']:
    d.layers.new(name)
m = d.modelspace()
m.add_lwpolyline([(0,0),(12,0),(12,8),(0,8)], close=True, dxfattribs={'layer':'CHIP_OUTLINE'})
# Union centreline strokes, observation chamber and ports into one connected filled region.
segments = [
    ([(1.2,2),(2.6,2),(2.6,4)], .2),
    ([(1.2,6),(2.6,6),(2.6,4)], .2),
    ([(2.6,4),(3.6,4),(3.6,3),(4.4,3),(4.4,5),(5.6,5),(5.6,3),(6.4,3),(6.4,4),(8.5,4)], .2),
    ([(8.4,4),(9.25,3),(10.8,3)], .2),
    ([(8.4,4),(9.25,5),(10.8,5)], .2),
]
polygons = [gdstk.rectangle((6.3,3.65),(8.5,4.35))]
for points,width in segments:
    polygons.extend(gdstk.FlexPath(points,width,joins='miter',ends='extended').to_polygons())
for x,y in [(1.2,2),(1.2,6),(10.8,3),(10.8,5)]:
    polygons.append(gdstk.ellipse((x,y),.45,tolerance=.0001))
areas = gdstk.boolean(polygons, [], 'or', precision=.00001)
assert len(areas) == 1, 'Fluidic concept must be connected'
for area in areas:
    m.add_lwpolyline(area.points,close=True,dxfattribs={'layer':'NO_DOT'})
for x,y in [(0.6,.6),(11.4,.6),(.6,7.4),(11.4,7.4)]:
    m.add_line((x-.15,y),(x+.15,y),dxfattribs={'layer':'ALIGNMENT'})
    m.add_line((x,y-.15),(x,y+.15),dxfattribs={'layer':'ALIGNMENT'})
d.saveas(out)
print(out)
