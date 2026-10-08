from pathlib import Path

script = (Path(__file__).resolve().parents[2] / 'resources/installers/mib-studio-qt.iss').read_text(encoding='utf-8')
check = script.split('function VCRuntimeNotInstalled: Boolean;', 1)[1].split('// If none of the checks pass', 1)[0]
assert 'ArchitecturesInstallIn64BitMode=x64compatible' in script
assert '{syswow64}' not in check, 'x86 runtime must not satisfy the x64 DLL fallback'
for dll in ('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll'):
    assert "FileExists(ExpandConstant('{sys}\\" + dll + "'))" in check, dll
print('x64 VC runtime fallback requires all three native DLLs')
