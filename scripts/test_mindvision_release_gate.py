#!/usr/bin/env python3
"""Regression guard for default-on MindVision builds (issue #338)."""

from __future__ import annotations

import os
import pathlib
import re
import stat
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
WINDOWS_SDK_URL = (
    "https://updates.yofo.bio/mindvision-sdk/"
    "MindVision-Camera-Platform-Setup2.1.10.195_202604021438.exe"
)
WINDOWS_SDK_SHA256 = "a62f58a8aef103d0061dc2c12b709d0655136cc488840e22361dff08adc4d4f4"
LINUX_SDK_URL = (
    "https://updates.yofo.bio/mindvision-sdk/"
    "linuxSDK_V2.1.0.49202602041120.tar.gz"
)
LINUX_SDK_SHA256 = "246d374dc7f91a8fa7120ceced020680a2b249bbfcf2d974d4e0ed0c04cc6313"
MACOS_SDK_URL = "https://updates.yofo.bio/mindvision-sdk/mac-sdk.rar"
MACOS_SDK_SHA256 = "ae3358bcb24a10275248ef5e7cc0c5507dbe804436112528b38c266eed140014"


class MindVisionReleaseGateTest(unittest.TestCase):
    def read(self, relative_path: str) -> str:
        return (ROOT / relative_path).read_text(encoding="utf-8")

    def test_every_desktop_platform_defaults_to_mindvision(self) -> None:
        options = self.read("cmake/MIBOptions.cmake")
        self.assertRegex(options, r"set\(MIB_ENABLE_MINDVISION_DEFAULT\s+ON\)")
        self.assertNotRegex(
            options,
            re.compile(r"MIB_ENABLE_MINDVISION_DEFAULT\s+OFF.*?if\(WIN32\)", re.DOTALL),
        )

        presets = self.read("CMakePresets.json")
        self.assertGreaterEqual(presets.count('"MIB_ENABLE_MINDVISION": "ON"'), 4)

    def test_provisioner_pins_and_validates_the_r2_sdk(self) -> None:
        provisioner = self.read("scripts/provision-mindvision-sdk.ps1")
        self.assertIn(WINDOWS_SDK_URL, provisioner)
        self.assertIn(WINDOWS_SDK_SHA256, provisioner)
        self.assertIn("CameraApiLoad.h", provisioner)
        self.assertIn("MVCAMSDK_X64.dll", provisioner)
        for api in (
            "CameraGetImageBufferPriority",
            "CameraSetExtTrigSignalType",
            "CameraSetStrobeMode",
            "CameraSetOutPutIOMode",
            "CameraSoftTrigger",
        ):
            self.assertIn(api, provisioner)
        backend_cmake = self.read("src/backend/CMakeLists.txt")
        self.assertIn("MIB_MINDVISION_USE_PRIORITY_API=1", backend_cmake)

    def test_unix_provisioner_pins_linux_and_macos_sdks(self) -> None:
        provisioner_path = ROOT / "scripts/provision-mindvision-sdk.sh"
        provisioner = provisioner_path.read_text(encoding="utf-8")
        # Windows filesystems do not expose the executable bit recorded by Git.
        # The permission is meaningful and testable only on POSIX checkouts.
        if os.name == "posix":
            self.assertTrue(provisioner_path.stat().st_mode & stat.S_IXUSR)
        for expected in (
            LINUX_SDK_URL,
            LINUX_SDK_SHA256,
            MACOS_SDK_URL,
            MACOS_SDK_SHA256,
            "CameraApi.h",
            "CameraGetImageBufferPriority",
            "CameraSetExtTrigSignalType",
            "CameraSetStrobeMode",
            "CameraSetOutPutIOMode",
            "CameraSoftTrigger",
            "libMVSDK.so",
            "libmvsdk.dylib",
        ):
            self.assertIn(expected, provisioner)

    def test_cmake_enables_and_links_mindvision_on_unix(self) -> None:
        dependencies = self.read("cmake/MIBDependencies.cmake")
        self.assertRegex(
            dependencies,
            re.compile(
                r"if\(MIB_ENABLE_MINDVISION AND NOT MIB_BUILD_PROCESSING_ONLY\).*?"
                r"set\(MIB_HAS_MINDVISION ON\)",
                re.DOTALL,
            ),
        )
        self.assertIn("CameraApi.h", dependencies)
        self.assertIn("libMVSDK.so", dependencies)
        self.assertIn("libmvsdk.dylib", dependencies)
        self.assertIn("build/vendor/mindvision-sdk/extracted", dependencies)

        backend_cmake = self.read("src/backend/CMakeLists.txt")
        self.assertIn("MIB_MINDVISION_LIBRARY", backend_cmake)

        # MindVisionCamera.cpp is SDK-header-free by design (issue #366): all
        # vendor calls go through the seam bound in MindVisionSdkReal.cpp.
        for relative_path in (
            "src/backend/camera/mindvision/MindVisionSdkReal.cpp",
            "src/backend/camera/mindvision/MindVisionApply.cpp",
            "src/backend/services/CameraControlService.cpp",
        ):
            with self.subTest(path=relative_path):
                self.assertIn("CameraApi.h", self.read(relative_path))

    def test_every_release_entrypoint_provisions_and_requires_mindvision(self) -> None:
        for relative_path in (
            ".github/workflows/build-windows.yml",
            ".github/workflows/release.yml",
            "release.ps1",
        ):
            with self.subTest(path=relative_path):
                content = self.read(relative_path)
                self.assertIn("provision-mindvision-sdk.ps1", content)
                self.assertIn("MIB_ENABLE_MINDVISION=ON", content)
                self.assertIn("MIB_MINDVISION_SDK_ROOT", content)
                self.assertIn("MIB_MINDVISION_RUNTIME_DIR", content)
                self.assertNotIn("MIB_ENABLE_MINDVISION=OFF", content)

    def test_every_full_ci_build_provisions_the_platform_sdk(self) -> None:
        for relative_path in (
            ".github/workflows/backend-ci.yml",
            ".github/workflows/sanitizers.yml",
            ".github/workflows/soak.yml",
        ):
            with self.subTest(path=relative_path):
                content = self.read(relative_path)
                self.assertIn("provision-mindvision-sdk.sh", content)
                self.assertNotIn("MIB_ENABLE_MINDVISION=OFF", content)

        processing_core = self.read(".github/workflows/python-wheel.yml")
        self.assertIn("provision-mindvision-sdk.ps1", processing_core)
        self.assertIn("provision-mindvision-sdk.sh", processing_core)


if __name__ == "__main__":
    unittest.main()
