"""Windows OS DLLs are never staged beside the Tauri exe (#851).

file(GET_RUNTIME_DEPENDENCIES) reported paths such as "C:\\WINDOWS\\system32/kernel32.dll";
the old POST_EXCLUDE regex matched only "/System32/", and 31 OS DLLs were packaged. These
tests run desktop/scripts/windows-system-dlls.cmake through its self-test driver with
`cmake -P`, so they run on any host. The real packaging run is the Windows candidate lane
(.github/workflows/desktop-windows-candidate.yml).
"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SELFTEST = ROOT / "desktop" / "scripts" / "windows-system-dlls-selftest.cmake"
RUNTIME = ROOT / "desktop" / "scripts" / "windows-runtime.cmake"
CMAKE = shutil.which("cmake")

SYSTEM_PATHS = [
    "C:\\WINDOWS\\system32/kernel32.dll",       # the shape seen on win11-agent
    "C:\\Windows\\System32\\ntdll.dll",
    "c:/windows/system32/USER32.dll",
    "C:/Windows/SysWOW64/gdi32.dll",
    "C:\\Windows\\WinSxS\\amd64_foo\\comctl32.dll",
    "C:\\Program Files\\x\\api-ms-win-core-synch-l1-2-0.dll",
    "D:/tools/API-MS-WIN-CRT-RUNTIME-L1-1-0.DLL",
    "D:/tools/ext-ms-win-ntuser-window-l1-1-0.dll",
]
APP_PATHS = [
    "C:/build/conan/opencv/bin/opencv_world4100.dll",
    "C:\\build\\hdf5\\bin\\hdf5.dll",
    "C:\\Users\\windows\\app\\zlib1.dll",       # "windows" in a path is not the OS directory
]


def run(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run([CMAKE, *args, "-P", str(SELFTEST)], capture_output=True, text=True)


def classified(mode: str, paths: list[str], system_root: str = "") -> dict[str, str]:
    result = run(f"-DMODE={mode}", f"-DPATHS={';'.join(paths)}", f"-DSYSTEM_ROOT={system_root}")
    if result.returncode != 0:
        raise AssertionError(result.stderr)
    lines = [line for line in result.stderr.splitlines() if " => " in line]
    return dict(line.rsplit(" => ", 1) for line in lines)


@unittest.skipIf(CMAKE is None, "cmake not on PATH")
class WindowsRuntimeFilter(unittest.TestCase):
    def test_post_exclude_regexes_match_any_case_and_either_separator(self) -> None:
        got = classified("regex", SYSTEM_PATHS + APP_PATHS)
        for path in SYSTEM_PATHS:
            self.assertEqual(got[path], "system", path)
        for path in APP_PATHS:
            self.assertEqual(got[path], "keep", path)

    def test_resolved_paths_under_system_root_are_dropped(self) -> None:
        got = classified("classify", SYSTEM_PATHS + APP_PATHS + ["E:\\WinRoot\\Custom\\foo.dll"],
                         system_root="e:\\winroot")
        for path in SYSTEM_PATHS + ["E:\\WinRoot\\Custom\\foo.dll"]:
            self.assertEqual(got[path], "system", path)
        for path in APP_PATHS:
            self.assertEqual(got[path], "keep", path)

    def test_staged_check_fails_on_an_os_dll(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            dest = Path(tmp) / "stage"
            dest.mkdir()
            for name in ("mib-studio-desktop.exe", "opencv_world4100.dll", "vcruntime140.dll", "msvcp140_1.dll"):
                (dest / name).write_bytes(b"")
            ok = run("-DMODE=check", f"-DDEST={dest}")
            self.assertEqual(ok.returncode, 0, ok.stderr)

            for bad in ("KERNEL32.dll", "api-ms-win-core-file-l1-1-0.dll", "ext-ms-win-foo-l1-1-0.dll"):
                (dest / bad).write_bytes(b"")
                failed = run("-DMODE=check", f"-DDEST={dest}")
                self.assertNotEqual(failed.returncode, 0, bad)
                self.assertIn(bad, failed.stderr)
                (dest / bad).unlink()

    def test_staged_check_uses_system32_but_allows_the_app_local_msvc_runtime(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "Windows"
            (root / "System32").mkdir(parents=True)
            for name in ("vendorhelper.dll", "vcruntime140.dll"):
                (root / "System32" / name).write_bytes(b"")
            dest = Path(tmp) / "stage"
            dest.mkdir()
            (dest / "vcruntime140.dll").write_bytes(b"")
            self.assertEqual(run("-DMODE=check", f"-DDEST={dest}", f"-DSYSTEM_ROOT={root}").returncode, 0)
            (dest / "vendorhelper.dll").write_bytes(b"")
            failed = run("-DMODE=check", f"-DDEST={dest}", f"-DSYSTEM_ROOT={root}")
            self.assertNotEqual(failed.returncode, 0)
            self.assertIn("vendorhelper.dll", failed.stderr)

    def test_packaging_script_uses_the_shared_filter_and_check(self) -> None:
        text = RUNTIME.read_text(encoding="utf-8")
        self.assertIn("POST_EXCLUDE_REGEXES ${MIB_WINDOWS_SYSTEM_DLL_REGEXES}", text)
        self.assertIn("mib_filter_windows_system_dlls(", text)
        self.assertIn('mib_check_staged_runtime("${DEST}"', text)


if __name__ == "__main__":
    unittest.main()
