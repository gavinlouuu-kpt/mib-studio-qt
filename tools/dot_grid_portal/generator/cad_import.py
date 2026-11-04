"""Geometry-preserving CAD import. Converter exit status alone is not proof."""
from pathlib import Path
import os
import shutil
import subprocess
import signal
import tempfile
import ezdxf
from ezdxf.acis import api as acis
from shapely.geometry import Polygon

ACIS_TYPES = {'REGION', 'BODY', '3DSOLID', 'SURFACE'}


def empty_bodies(doc):
    return [e for e in doc.modelspace() if e.dxftype() in ACIS_TYPES and not (e.sat or e.sab)]


def convert(source, out):
    """Prefer configured preservation converter; never accept empty ACIS bodies.

    CAD_ODA_EXECUTABLE is an operator-owned executable (not a shell command).
    A headless wrapper may invoke ODA through xvfb. No customer paths/arguments
    enter a shell; only a fixed-name copy in a fresh directory is processed.
    """
    oda = os.environ.get('CAD_ODA_EXECUTABLE')
    exe = oda or shutil.which('dwg2dxf')
    if not exe:
        raise ValueError('CAD_CONVERTER_UNAVAILABLE: DWG conversion unavailable')
    target = out / 'converted.dxf'
    target.unlink(missing_ok=True)
    with tempfile.TemporaryDirectory(prefix='cad-convert-', dir=out) as folder:
        base = Path(folder)
        if oda:
            src, dst = base / 'input', base / 'output'
            src.mkdir(); dst.mkdir()
            shutil.copyfile(source, src / 'device.dwg')
            args = [exe, str(src), str(dst), 'ACAD2000', 'DXF', '0', '0', '*.dwg']
            output = dst / 'device.dxf'
        else:
            output = base / 'device.dxf'
            args = [exe, '-o', str(output), str(source.resolve())]
        try:
            with tempfile.TemporaryFile() as log:
                with subprocess.Popen(args, stdout=log, stderr=log, start_new_session=True) as process:
                    try:
                        process.wait(timeout=90)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.wait()
                        raise
                    result_code = process.returncode
        except (OSError, subprocess.TimeoutExpired) as ex:
            raise ValueError('CAD_CONVERSION_FAILED: Conversion did not complete') from ex
        if result_code or not output.is_file():
            raise ValueError('CAD_CONVERSION_FAILED: Conversion did not complete')
        if output.stat().st_size > 128 * 1024**2:
            raise ValueError('CAD_CONVERSION_FAILED: Expanded drawing exceeds import limit')
        doc = ezdxf.readfile(output)
        if empty_bodies(doc):
            raise ValueError('CAD_GEOMETRY_LOSS: Converter lost filled-region geometry; original upload retained')
        shutil.copyfile(output, target)
    return target, 'ODA File Converter' if oda else 'GNU LibreDWG dwg2dxf'


def linked(first, attr, circular=False):
    seen = set(); item = first
    while not item.is_none:
        if id(item) in seen:
            if circular and item is first:
                return
            raise ValueError('CAD_REGION_UNSUPPORTED: Invalid region topology')
        seen.add(id(item))
        if len(seen) > 20000:
            raise ValueError('CAD_REGION_UNSUPPORTED: Region topology exceeds limit')
        yield item
        item = getattr(item, attr)
    if circular:
        raise ValueError('CAD_REGION_UNSUPPORTED: Region boundary is open')


def flatten_regions(doc):
    """Extract only fully verified planar straight-edge REGIONs, never partial meshes.

    Other ACIS surfaces and inner loops require a full geometry kernel; reject
    rather than silently omit faces/holes (ezdxf.mesh_from_body can omit them).
    Text remains in the delivered CAD but is not treated as device geometry.
    """
    msp = doc.modelspace(); converted = 0
    for e in list(msp.query('REGION')):
        if not (e.sat or e.sab):
            raise ValueError('CAD_GEOMETRY_LOSS: Filled-region boundary data is missing')
        try:
            bodies = acis.load_dxf(e)
            polygons = []
            for body in bodies:
                before = len(polygons)
                if body.lump.is_none or not body.wire.is_none:
                    raise ValueError('Standalone wires')
                for lump in linked(body.lump, 'next_lump'):
                    if lump.shell.is_none: raise ValueError('Missing shell')
                    for shell in linked(lump.shell, 'next_shell'):
                        if shell.face.is_none: raise ValueError('Missing face')
                        for face in linked(shell.face, 'next_face'):
                            if face.surface.type != 'plane-surface' or face.loop.is_none or not face.loop.next_loop.is_none:
                                raise ValueError('Curved face or inner loop')
                            edges = list(linked(face.loop.coedge, 'next_coedge', True))
                            points = []
                            ends = []
                            for co in edges:
                                edge = co.edge
                                if edge.curve.type != 'straight-curve':
                                    raise ValueError('Curved edge')
                                a, b = edge.start_vertex.point.location, edge.end_vertex.point.location
                                if co.sense: a, b = b, a
                                points.append(a); ends.append(b)
                            if len(points) < 3 or len(points) > 2000 or any(not b.isclose(a, abs_tol=1e-7) for b,a in zip(ends,points[1:]+points[:1])):
                                raise ValueError('Disconnected boundary')
                            if not body.transform.is_none:
                                points = list(body.transform.matrix.transform_vertices(points))
                            if any(abs(p.z) > 1e-7 for p in points):
                                raise ValueError('Non-XY region')
                            p = [(p.x,p.y) for p in points]
                            poly = Polygon(p)
                            if not poly.is_valid or poly.area < 1e-9: raise ValueError('Invalid polygon')
                            polygons.append(p)
                if len(polygons) == before: raise ValueError('Empty body')
            if not polygons:
                raise ValueError('No complete faces')
        except Exception as ex:
            raise ValueError('CAD_REGION_UNSUPPORTED: A filled region needs a geometry-kernel import') from ex
        for p in polygons:
            msp.add_lwpolyline(p, close=True, dxfattribs={'layer':e.dxf.layer, 'color':e.dxf.color})
        msp.delete_entity(e); converted += 1
    return converted
