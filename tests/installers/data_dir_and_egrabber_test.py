"""Installer regressions from the rig check v1-02 (findings F1 and F3).

F1: neither installer creates {app}\\data any more; the app keeps its state in
    %LOCALAPPDATA%\\MIB_Studio_Qt (backend/app/DataDirectory.h).
F3: the eGrabber [Run] step only runs when eGrabber is not installed yet, and
    the task stays ticked by default on a new PC.
"""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[2] / 'resources/installers'
setup = (root / 'mib-studio-qt.iss').read_text(encoding='utf-8')
update = (root / 'mib-studio-qt-update.iss').read_text(encoding='utf-8')


def code_lines(script):
    """Lines that are not Inno comments (';' starts a comment line)."""
    return [line for line in script.splitlines() if line.strip() and not line.lstrip().startswith(';')]


for name, script in (('setup', setup), ('update', update)):
    for line in code_lines(script):
        assert not re.search(r'\{app\}\\data', line, re.IGNORECASE), f'{name}: still references {{app}}\\data: {line}'

run_entries = [line for line in code_lines(setup) if 'Tasks: installegrabber' in line and line.startswith('Filename:')]
assert len(run_entries) == 1, run_entries
assert 'Check: EGrabberNotInstalled' in run_entries[0], 'eGrabber [Run] must skip an installed eGrabber'

task = [line for line in code_lines(setup) if line.startswith('Name: "installegrabber"')]
assert len(task) == 1, task
assert 'unchecked' not in task[0].lower(), 'eGrabber task stays ticked by default on a new PC'

body = setup.split('function EGrabberNotInstalled: Boolean;', 1)[1].split('end;\n\nprocedure', 1)[0]
assert "RegQueryStringValue(HKEY_LOCAL_MACHINE, 'SOFTWARE\\Euresys\\eGrabber', 'Version', Version)" in body
assert "DirExists(ExpandConstant('{commonpf64}\\Euresys\\eGrabber\\bin'))" in body
assert 'Result := True;' in body, 'not found -> install'
print('installers: no {app}\\data; eGrabber [Run] skips an installed eGrabber; task ticked by default')
