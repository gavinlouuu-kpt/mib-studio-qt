"""Build a focused native registry/decoder acceptance test; does not build the desktop."""
import argparse
from pathlib import Path
import shlex
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('registry')
a = p.parse_args()
root = Path(__file__).resolve().parents[3]
version = (root / 'scripts/dot_grid/dotgrid/VERSION').read_text(encoding='utf-8').strip()
with tempfile.TemporaryDirectory(prefix='mib-native-map-') as temp:
    binary = str(Path(temp) / 'native-map-test')
    flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'opencv4', 'spdlog'], text=True))
    sources = [root / 'src/backend/processing' / ('DotGrid' + name + '.cpp') for name in ('Registry', 'Codebook', 'Codec', 'Decoder')]
    sources.append(root / 'src/backend/processing/BundledProcessingKernel.cpp')
    subprocess.run(['g++', '-std=c++17', '-O1', '-pthread', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', '-DMIB_PROCESSING_BUNDLED_CONTRACT=1', f'-DMIB_DOTGRID_CORE_VERSION="{version}"', '-I' + str(root / 'include'), str(Path(__file__).with_name('native_map_test.cpp')), *map(str, sources), *flags, '-o', binary], check=True)
    subprocess.run([binary, str(Path(a.registry).resolve())], check=True)
