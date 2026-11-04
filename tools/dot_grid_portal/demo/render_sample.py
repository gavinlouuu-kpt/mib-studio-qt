"""Render illustrations from actual source and generated DXF, never invented dots."""
import json,sys,zipfile
from pathlib import Path
import ezdxf
root=Path(__file__).resolve().parents[1]/'web/public/demo'
source=ezdxf.readfile(root/'sorter-concept.dxf')
generated=ezdxf.readfile(sys.argv[1])
manifest=json.loads(zipfile.ZipFile(sys.argv[2]).read('manifest.json'))
def geom(doc,scale=1,dots=False,window=None):
    a=[]
    for e in doc.modelspace():
        isdot=e.dxf.layer=='DOTGRID'
        if isdot and not dots:continue
        color='#d87927' if isdot else '#176c67'
        if e.dxftype()=='LINE':
            p,q=e.dxf.start,e.dxf.end
            a.append(f'<path d="M{p.x*scale},{p.y*scale} L{q.x*scale},{q.y*scale}" stroke="{color}" stroke-width="10" fill="none"/>')
        elif e.dxftype()=='LWPOLYLINE':
            ps=' '.join(f'{x*scale},{y*scale}' for x,y in e.get_points('xy'))
            a.append(f'<polygon points="{ps}" stroke="{color}" stroke-width="12" fill="none"/>')
        elif e.dxftype()=='CIRCLE':
            x,y,r=e.dxf.center.x*scale,e.dxf.center.y*scale,e.dxf.radius*scale
            if window and not(window[0]-r<=x<=window[0]+window[2]+r and window[1]-r<=y<=window[1]+window[3]+r):continue
            a.append(f'<circle cx="{x}" cy="{y}" r="{r}" fill="{color if isdot else "none"}" stroke="{color}" stroke-width="{0 if isdot else 10}"/>')
    return ''.join(a)
def svg(name,view,body):
    (root/name).write_text(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{view}" role="img"><rect x="-2000" y="-2000" width="18000" height="14000" fill="#f5faf8"/>{body}</svg>')
labels=[(650,1250,'Two inlet ports'),(3400,6500,'Serpentine mixer'),(6600,2500,'Observation region'),(9550,6500,'Two outlets')]
body=geom(source,1000)
for x,y,text in labels:body+=f'<text x="{x}" y="{y}" font-family="sans-serif" font-size="280" fill="#173d3a">{text}</text>'
body+='<rect x="6600" y="3200" width="2200" height="1600" stroke="#d87927" stroke-width="30" stroke-dasharray="80 60" fill="none"/><text x="400" y="8550" font-family="sans-serif" font-size="280" fill="#173d3a">12 × 8 mm · original concept CAD · orange box = magnified view</text>'
svg('device.svg','-200 -200 12400 9200',body)
window=(6600,3200,2200,1600)
svg('before.svg','6600 3200 2200 1600',geom(generated,window=window))
svg('after.svg','6600 3200 2200 1600',geom(generated,dots=True,window=window))
(root/'sample.json').write_text(json.dumps({'design_id':manifest['design_id'],'dot_count':manifest['dot_count'],'seed':manifest['seed'],'source_sha256':manifest['source_sha256'],'codec_contract':manifest['codec_contract'],'extent_um':manifest['extent_um'],'illustration':'Actual exported DXF geometry, cropped at x=6600..8800 and y=3200..4800 um. Display uses CAD XY coordinates (positive Y downward in this SVG).','fabrication_approved':False},indent=2)+'\n')
print(manifest['dot_count'])
