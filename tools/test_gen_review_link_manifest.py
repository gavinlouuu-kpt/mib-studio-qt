#!/usr/bin/env python3
"""Unit tests for tools/gen_review_link_manifest.py on synthetic Ninja trees
(macOS clang and Windows MSVC link lines), so the parser is covered on every
host, not only on the macOS / Windows runners that use it."""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import gen_review_link_manifest as gen  # noqa: E402

MAC_NINJA = """\
ninja_required_version = 1.5
build src/backend/CMakeFiles/mib_review_link_probe.dir/__/__/tools/review_link_probe/main.cpp.o: CXX_COMPILER__mib_review_link_probe_unscanned_Release /src/tools/review_link_probe/main.cpp
  FLAGS = -O3
build mib_review_link_probe: CXX_EXECUTABLE_LINKER__mib_review_link_probe_Release src/backend/CMakeFiles/mib_review_link_probe.dir/__/__/tools/review_link_probe/main.cpp.o | libmib_review_core.a libmib_processing.a
  FLAGS = -O3 -DNDEBUG -arch arm64
  LINK_FLAGS = -Wl,-search_paths_first -Wl,-headerpad_max_install_names
  LINK_LIBRARIES = libmib_review_core.a  libmib_processing.a  -ldl  /Users/r/.conan2/p/b/spdlo1/p/lib/libspdlog.a  /Users/r/.conan2/p/b/openc2/p/lib/libopencv_imgcodecs.a  -framework CoreFoundation  -framework AppKit  /Users/r/.conan2/p/b/hdf53/p/lib/libhdf5.a  -lm
  OBJECT_DIR = src/backend/CMakeFiles/mib_review_link_probe.dir
  TARGET_FILE = mib_review_link_probe
"""

WIN_NINJA = r"""
build mib_review_link_probe.exe: CXX_EXECUTABLE_LINKER__mib_review_link_probe_Release src\backend\CMakeFiles\mib_review_link_probe.dir\__\__\tools\review_link_probe\main.cpp.obj | mib_review_core.lib mib_processing.lib || mib_processing.lib
  FLAGS = /DWIN32 /D_WINDOWS /EHsc /O2 /Ob2 /DNDEBUG -MD
  LINK_FLAGS = /machine:x64 /INCREMENTAL:NO /subsystem:console
  LINK_LIBRARIES = mib_review_core.lib  mib_processing.lib  wintrust.lib  crypt32.lib  C$:\Users\runneradmin\.conan2\p\b\opencfa\p\lib\opencv_core4120.lib  "C$:\Program Files\Odd Dir\hdf5.lib"  kernel32.lib user32.lib
  LINK_PATH = -LIBPATH:C$:\Users\runneradmin\.conan2\p\b\zlib\p\lib
  TARGET_FILE = mib_review_link_probe.exe
"""


def write_tree(root: Path, ninja: str, command: str):
    (root / "build.ninja").write_text(ninja, encoding="utf-8")
    (root / "compile_commands.json").write_text(json.dumps([
        {"directory": str(root), "file": "/src/src/backend/review/ReviewSession.cpp", "command": command},
    ]), encoding="utf-8")


class Parsing(unittest.TestCase):
    def test_windows_splitter(self):
        self.assertEqual(gen.split_windows(r'a "C:\Program Files\x.lib" -DV=\"1.0\" C:\a\b\\'), ["a", r"C:\Program Files\x.lib", '-DV="1.0"', "C:\\a\\b\\\\"])

    def test_build_line_with_escaped_colons(self):
        outs, rule = gen.split_build_line(r"build C$:\out\mib_review_link_probe.exe: CXX_EXECUTABLE_LINKER__x a.obj")
        self.assertEqual(outs, [r"C:\out\mib_review_link_probe.exe"])
        self.assertEqual(rule, "CXX_EXECUTABLE_LINKER__x")

    def run_gen(self, root: Path, windows: bool) -> dict:
        args = [sys.executable, str(HERE / "gen_review_link_manifest.py"), "--build-dir", str(root)]
        if windows:
            args.append("--windows")
        subprocess.run(args, check=True, capture_output=True)
        return json.loads((root / "mib-bridge-link-manifest.json").read_text())

    def test_macos_link_line(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            write_tree(root, MAC_NINJA, '/usr/bin/c++ -DMIB_PROCESSING_CORE_VERSION=\\"0.1.0\\" -DMIB_PROCESSING_BUNDLED_CONTRACT=1 -I/src/include -isystem /Users/r/.conan2/p/b/openc2/p/include/opencv4 -O3 -std=gnu++17 -o x.o -c /src/src/backend/review/ReviewSession.cpp')
            m = self.run_gen(root, windows=False)
            self.assertEqual(m["format"], "mib-review-link-v1")
            self.assertEqual([(l["kind"], l["name"]) for l in m["link"]], [
                ("static", "mib_review_core"), ("static", "mib_processing"), ("dylib", "dl"),
                ("static", "spdlog"), ("static", "opencv_imgcodecs"),
                ("framework", "CoreFoundation"), ("framework", "AppKit"),
                ("static", "hdf5"), ("dylib", "m"),
            ])
            self.assertEqual(m["link"][0]["dir"], str(root.resolve()))
            self.assertIn('MIB_PROCESSING_CORE_VERSION="0.1.0"', m["defines"])
            self.assertIn("/Users/r/.conan2/p/b/openc2/p/include/opencv4", m["include_dirs"])

    def test_windows_link_line(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            for lib in ("mib_review_core.lib", "mib_processing.lib"):
                (root / lib).write_bytes(b"")
            write_tree(root, WIN_NINJA, r'C:\VS\cl.exe /nologo /TP -DMIB_PROCESSING_CORE_VERSION=\"0.1.0\" -IC:\src\include -external:IC:\conan\opencv\include -external:W0 /DWIN32 /EHsc /O2 -MD -std:c++17 /FoX.obj /FdX.pdb /FS -c C:\src\src\backend\review\ReviewSession.cpp')
            # compile_commands stores the file with forward or back slashes.
            db = json.loads((root / "compile_commands.json").read_text())
            db[0]["file"] = r"C:\src\src\backend\review\ReviewSession.cpp"
            (root / "compile_commands.json").write_text(json.dumps(db))
            m = self.run_gen(root, windows=True)
            kinds = [(l["kind"], l["name"]) for l in m["link"]]
            self.assertEqual(kinds, [
                ("static", "mib_review_core"), ("static", "mib_processing"), ("dylib", "wintrust"), ("dylib", "crypt32"),
                ("static", "opencv_core4120"), ("static", "hdf5"), ("dylib", "kernel32"), ("dylib", "user32"),
            ])
            self.assertEqual(m["link"][5]["dir"], r"C:\Program Files\Odd Dir")
            self.assertIn(r"C:\Users\runneradmin\.conan2\p\b\zlib\p\lib", m["search_dirs"])
            self.assertIn(r"C:\conan\opencv\include", m["include_dirs"])
            self.assertIn('MIB_PROCESSING_CORE_VERSION="0.1.0"', m["defines"])
            self.assertIn("WIN32", m["defines"])

    def test_missing_probe_fails(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            write_tree(root, "build x: phony\n", "c++ -c /src/src/backend/review/ReviewSession.cpp")
            r = subprocess.run([sys.executable, str(HERE / "gen_review_link_manifest.py"), "--build-dir", str(root)], capture_output=True)
            self.assertEqual(r.returncode, 1)


if __name__ == "__main__":
    unittest.main()
