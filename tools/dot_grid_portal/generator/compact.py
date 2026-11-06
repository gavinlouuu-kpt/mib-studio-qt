"""Compact delivery without changing pattern positions or device geometry."""
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile


def compact_exports(folder):
    import klayout.db as kdb
    folder = Path(folder)
    layout = kdb.Layout()
    layout.read(str(folder / 'dotgrid_layer.gds'))
    options = kdb.SaveLayoutOptions()
    options.format = 'OASIS'
    options.oasis_compression_level = 10
    layout.write(str(folder / 'dotgrid_layer.oas'), options)
    exports = ['dotgrid_layer.oas']
    # Use the already provisioned preservation converter; never silently rename DXF.
    oda = os.environ.get('CAD_ODA_EXECUTABLE')
    if oda:
        with tempfile.TemporaryDirectory(prefix='mask-export-') as tmp:
            src, dst = Path(tmp)/'input', Path(tmp)/'output'
            src.mkdir(); dst.mkdir()
            shutil.copyfile(folder/'design_with_dotgrid.dxf', src/'mask.dxf')
            args = [oda, str(src), str(dst), 'ACAD2018', 'DWG', '0', '1', '*.dxf']
            with tempfile.TemporaryFile() as log:
                with subprocess.Popen(args, stdout=log, stderr=log, start_new_session=True) as process:
                    try:
                        process.wait(timeout=90)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.wait()
                        raise ValueError('CAD_EXPORT_FAILED: compact CAD export timed out')
            output = dst/'mask.dwg'
            header = b''
            if output.is_file():
                with output.open('rb') as stream: header = stream.read(6)
            if process.returncode or header != b'AC1032':
                raise ValueError('CAD_EXPORT_FAILED: compact CAD export failed')
            shutil.copyfile(output, folder/'design_with_dotgrid.dwg')
            exports.insert(0, 'design_with_dotgrid.dwg')
    return exports
