#!/usr/bin/env python3
"""Bundle an already-built Tauri binary with distro-derived native dependencies.
Build on the oldest supported Debian/Ubuntu target; packages are not universal.
"""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--debug', action='store_true')
args = parser.parse_args()
desktop = Path(__file__).resolve().parents[1]
profile = 'debug' if args.debug else 'release'
binary = desktop / 'src-tauri' / 'target' / profile / 'mib-studio-desktop'
if not binary.is_file():
    parser.error(f'Build the custom-protocol {profile} binary first: {binary}')
with tempfile.TemporaryDirectory(prefix='mib-deb-dependencies-') as directory:
    staging = Path(directory)
    (staging / 'debian').mkdir()
    (staging / 'debian/control').write_text('Source: mib-studio-desktop\nSection: science\nPriority: optional\nMaintainer: MIB Studio\n\nPackage: mib-studio-desktop\nArchitecture: any\nDescription: MIB Studio desktop\n')
    result = subprocess.check_output(['dpkg-shlibdeps', '-O', '-e' + str(binary)], cwd=staging, text=True)
    dependencies = next(line.split('=', 1)[1].split(', ') for line in result.splitlines() if line.startswith('shlibs:Depends='))
    if not dependencies:
        raise RuntimeError('No native dependencies resolved')
    config = staging / 'bundle.json'
    config.write_text(json.dumps({'bundle': {'linux': {'deb': {'depends': dependencies}}}}))
    subprocess.run(['npm', 'run', 'tauri', '--', 'bundle', '--bundles', 'deb', '--config', str(config), *(['--debug'] if args.debug else [])], cwd=desktop, check=True)
