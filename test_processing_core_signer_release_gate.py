#!/usr/bin/env python3
"""Regression coverage for the production processing-core signer trust gate."""

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent
OPTIONS_FILE = REPO_ROOT / "cmake" / "MIBOptions.cmake"
PIN_NAME = "MIB_PROCESSING_CORE_SIGNER_SPKI_SHA256"
REQUIRE_NAME = "MIB_REQUIRE_PROCESSING_CORE_SIGNER_SPKI"
APP_MIN_NAME = "MIB_PROCESSING_CORE_APP_MIN_VERSION"
APP_MAX_NAME = "MIB_PROCESSING_CORE_APP_MAX_VERSION"
APP_REQUIRE_NAME = "MIB_REQUIRE_PROCESSING_CORE_APP_COMPAT"


class ProcessingCoreSignerCMakeTest(unittest.TestCase):
    def run_options(
        self,
        *,
        pin: str = "",
        required: bool = False,
        expected: str | None = None,
        app_min: str = "",
        app_max: str = "",
        require_app_compat: bool = False,
    ) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as temporary_directory:
            script = Path(temporary_directory) / "check.cmake"
            expected_check = ""
            if expected is not None:
                expected_check = f"""
if(NOT \"${{{PIN_NAME}}}\" STREQUAL \"{expected}\")
    message(FATAL_ERROR \"normalized signer pin was '${{{PIN_NAME}}}'\")
endif()
"""
            script.write_text(
                f'include([[{OPTIONS_FILE.as_posix()}]])\n{expected_check}',
                encoding="utf-8",
            )
            return subprocess.run(
                [
                    "cmake",
                    "-DPROJECT_VERSION=1.0.0",
                    f"-D{PIN_NAME}={pin}",
                    f"-D{REQUIRE_NAME}={'ON' if required else 'OFF'}",
                    f"-D{APP_MIN_NAME}={app_min}",
                    f"-D{APP_MAX_NAME}={app_max}",
                    f"-D{APP_REQUIRE_NAME}={'ON' if require_app_compat else 'OFF'}",
                    "-P",
                    str(script),
                ],
                capture_output=True,
                text=True,
                check=False,
            )

    def test_development_build_may_leave_pin_empty(self) -> None:
        result = self.run_options(expected="")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_production_build_requires_pin(self) -> None:
        result = self.run_options(required=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("production build requires", result.stdout + result.stderr)

    def test_pin_must_be_exactly_64_hex_characters(self) -> None:
        for invalid in ("a" * 63, "a" * 65, "g" * 64, "0x" + "a" * 64):
            with self.subTest(pin=invalid):
                result = self.run_options(pin=invalid)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("exactly 64 hexadecimal", result.stdout + result.stderr)

    def test_valid_pin_is_normalized_to_lowercase(self) -> None:
        result = self.run_options(pin="ABCDEF12" * 8, required=True, expected="abcdef12" * 8)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_release_requires_explicit_ordered_app_compatibility_bounds(self) -> None:
        missing = self.run_options(require_app_compat=True)
        self.assertNotEqual(missing.returncode, 0)
        self.assertIn("explicit app compatibility bounds", missing.stdout + missing.stderr)

        reversed_bounds = self.run_options(app_min="2.0.0", app_max="1.0.0")
        self.assertNotEqual(reversed_bounds.returncode, 0)
        self.assertIn("minimum version must not exceed", reversed_bounds.stdout + reversed_bounds.stderr)

        valid = self.run_options(
            app_min="1.0.6", app_max="1.0.99", require_app_compat=True,
        )
        self.assertEqual(valid.returncode, 0, valid.stdout + valid.stderr)


class ProcessingCoreSignerReleaseWiringTest(unittest.TestCase):
    def read(self, relative_path: str) -> str:
        return (REPO_ROOT / relative_path).read_text(encoding="utf-8")

    def test_desktop_workflows_use_repository_pin_and_required_cmake_gate(self) -> None:
        for workflow in (
            ".github/workflows/release.yml",
            ".github/workflows/build-windows.yml",
        ):
            with self.subTest(workflow=workflow):
                content = self.read(workflow)
                self.assertIn("vars.MIB_PROCESSING_CORE_SIGNER_SPKI_SHA256", content)
                self.assertIn("-DMIB_REQUIRE_PROCESSING_CORE_SIGNER_SPKI=ON", content)
                self.assertIn("-DMIB_PROCESSING_CORE_SIGNER_SPKI_SHA256=", content)

    def test_manual_workflow_validates_before_version_mutation(self) -> None:
        content = self.read(".github/workflows/build-windows.yml")
        self.assertLess(
            content.index("name: Validate processing-core signer trust pin"),
            content.index("name: Determine prospective version"),
        )

    def test_local_release_reads_repo_pin_before_bump_and_reconfigures(self) -> None:
        content = self.read("release.ps1")
        preflight = content[: content.index("# --- Step 1: Bump version ---")]
        self.assertIn("if (-not $SkipBuild) {", preflight)
        self.assertNotIn("if ($Push -and -not $SkipBuild) {", preflight)
        self.assertLess(
            content.index("gh variable get MIB_PROCESSING_CORE_SIGNER_SPKI_SHA256"),
            content.index("# --- Step 1: Bump version ---"),
        )
        self.assertIn("-DMIB_REQUIRE_PROCESSING_CORE_SIGNER_SPKI=ON", content)
        self.assertIn("-DMIB_PROCESSING_CORE_SIGNER_SPKI_SHA256=", content)

    def test_native_release_compares_actual_der_spki_to_repository_pin(self) -> None:
        content = self.read(".github/workflows/python-wheel.yml")
        self.assertIn("ExportSubjectPublicKeyInfo", content)
        self.assertIn("Get-AuthenticodeSignature", content)
        self.assertIn("actualSignerSpki -ne $approvedSignerSpki", content)

    def test_native_release_assets_are_staged_flat_from_release_configuration(self) -> None:
        content = self.read(".github/workflows/python-wheel.yml")
        self.assertIn("build\\Release\\$stem.dll", content)
        self.assertIn("build\\Release\\$stem.json", content)
        self.assertIn("mib-processing-native-dist", content)
        self.assertIn("${{ runner.temp }}/mib-processing-native-dist/*", content)
        self.assertIn("Validate flat release asset set", content)
        self.assertIn("nested release asset", content)
        self.assertNotIn("Get-ChildItem build -Recurse -File -Filter \"$stem.dll\"", content)

    def test_linux_release_wheels_cover_supported_manylinux_architectures(self) -> None:
        content = self.read(".github/workflows/python-wheel.yml")
        self.assertIn("pypa/cibuildwheel@v2.22.0", content)
        for architecture in ("X86_64", "AARCH64"):
            self.assertIn(
                f"CIBW_MANYLINUX_{architecture}_IMAGE:",
                content,
            )
        self.assertIn("MIB_BUILD_PROCESSING_ONLY", self.read("bindings/python/pyproject.toml"))
        self.assertIn("python:3.12-slim", content)
        self.assertIn("Verify portable wheel in Biowork production base", content)
        self.assertIn("manylinux_2_28_(x86_64|aarch64)\\.whl", content)
        self.assertIn("len(wheels) != 8", content)
        self.assertNotIn("NOT an auditwheel/manylinux-portable wheel", content)

    def test_native_release_uses_independent_fixtures_and_audited_private_dependencies(self) -> None:
        content = self.read(".github/workflows/python-wheel.yml")
        self.assertIn("cmake -S tests/processing/fixtures", content)
        self.assertIn("mib_processing_fixture_throwing.dll", content)
        self.assertIn("/exports", content)
        self.assertIn("/dependents", content)
        self.assertIn("^(Qt|hdf5|opencv|python|mib_studio)", content)
        self.assertIn('opencv/*:shared=False', content)
        self.assertIn("test-processing-core-authenticode.ps1", content)

    def test_production_signing_is_isolated_from_unsigned_build_artifacts(self) -> None:
        content = self.read(".github/workflows/python-wheel.yml")
        self.assertIn("environment: Production", content)
        self.assertIn("mib_processing-native-windows-x86_64-unsigned", content)
        self.assertIn("name: mib_processing-native-windows-x86_64\n", content)
        self.assertIn("WINDOWS_SIGNING_CERTIFICATE_BASE64", content)
        self.assertIn("WINDOWS_SIGNING_CERTIFICATE_PASSWORD", content)

    def test_release_channel_and_app_compatibility_are_explicit(self) -> None:
        content = self.read(".github/workflows/python-wheel.yml")
        self.assertIn('channel = "stable" if re.fullmatch', content)
        self.assertIn("processing-core-registry-${{ needs.validate-source-version.outputs.channel }}", content)
        self.assertIn('--channel "$PROCESSING_CHANNEL"', content)
        self.assertIn("vars.MIB_PROCESSING_CORE_APP_MIN_VERSION", content)
        self.assertIn("vars.MIB_PROCESSING_CORE_APP_MAX_VERSION", content)
        self.assertIn("-DMIB_REQUIRE_PROCESSING_CORE_APP_COMPAT=", content)

    def test_authenticated_promotion_workflow_verifies_the_public_pointer(self) -> None:
        content = self.read(".github/workflows/processing-core-promote.yml")
        self.assertIn("workflow_dispatch:", content)
        self.assertIn("environment: Production", content)
        self.assertIn("--promote-version", content)
        self.assertIn("--upload-method s3", content)
        self.assertIn("verify-processing-core-manifest.py", content)
        self.assertIn("resolved version", content)
        self.assertIn("processing-core-registry-${{ inputs.channel }}", content)


class DesktopReleaseSafetyWiringTest(unittest.TestCase):
    def read(self, relative_path: str) -> str:
        return (REPO_ROOT / relative_path).read_text(encoding="utf-8")

    def test_local_release_uses_only_fresh_exact_version_installers(self) -> None:
        content = self.read("release.ps1")
        self.assertIn('"MIB_Studio_Qt_Setup_v$newVersion.exe"', content)
        self.assertIn('"MIB_Studio_Qt_Update_v$newVersion.exe"', content)
        self.assertNotIn('MIB_Studio_Qt_Setup_v*.exe', content)
        self.assertNotIn('MIB_Studio_Qt_Update_v*.exe', content)
        self.assertIn("Remove-Item -Force", content)
        self.assertIn("ERROR: Full installer build failed", content)
        self.assertIn("ERROR: Update package build failed", content)

    def test_local_release_dry_run_calculates_prospective_version(self) -> None:
        content = self.read("release.ps1")
        calculate = content.index("# Calculate the prospective version without mutating the tree")
        dry_run = content.index("if ($DryRun) {")
        self.assertLess(calculate, dry_run)
        self.assertIn("resolve_desktop_release_version.py", content)
        self.assertIn("$newVersion = [string]$versionInfo.next_version", content)
        self.assertIn("[DRY RUN] Prospective version:", content)

    def test_tag_dispatch_validates_and_checks_out_exact_tag(self) -> None:
        content = self.read(".github/workflows/release.yml")
        validate = content.index("name: Validate requested release tag")
        checkout = content.index("name: Checkout requested release tag")
        extract = content.index("name: Extract version from checked-out tag")
        self.assertLess(validate, checkout)
        self.assertLess(checkout, extract)
        self.assertIn("ref: ${{ steps.release-ref.outputs.ref }}", content)
        self.assertIn("git rev-list -n 1 \"$TAG\"", content)
        self.assertIn("git rev-parse HEAD", content)

    def test_tag_release_consumes_only_exact_versioned_installers(self) -> None:
        content = self.read(".github/workflows/release.yml")
        self.assertIn('echo "artifact_version=$ARTIFACT_VERSION"', content)
        self.assertIn(
            'build/dist/MIB_Studio_Qt_Setup_v${{ steps.version.outputs.artifact_version }}.exe',
            content,
        )
        self.assertIn(
            'build/dist/MIB_Studio_Qt_Update_v${{ steps.version.outputs.artifact_version }}.exe',
            content,
        )
        self.assertNotIn("MIB_Studio_Qt_Setup_v*.exe", content)
        self.assertNotIn("MIB_Studio_Qt_Update_v*.exe", content)
        self.assertNotIn("Select-Object -First 1", content)

    def test_manual_release_mutates_git_only_after_all_build_gates(self) -> None:
        content = self.read(".github/workflows/build-windows.yml")
        determine_start = content.index("name: Determine prospective version")
        determine_end = content.index("name: Install Conan")
        determine = content[determine_start:determine_end]
        self.assertNotIn("git commit", determine)
        self.assertNotIn("git tag -a", determine)
        self.assertNotIn("git push", determine)

        tests = content.index("name: Run tests")
        package = content.index("name: Build update package")
        publish_refs = content.index("name: Publish verified release tag")
        self.assertLess(tests, publish_refs)
        self.assertLess(package, publish_refs)
        publish_block = content[publish_refs:]
        self.assertIn("git tag", publish_block)
        # Tag-first flow: only the release tag ref is ever pushed. main is
        # branch-protected (PR + required checks) and rejects direct pushes,
        # including the workflow's own GITHUB_TOKEN — the fallback-version
        # bump rides a sync PR into develop instead of a branch push.
        self.assertIn('git push origin "refs/tags/$env:RELEASE_TAG"', publish_block)
        self.assertNotIn("HEAD:main", content)
        self.assertNotIn("git push --atomic", content)
        sync_pr = content.index("name: Open fallback-version sync PR into develop")
        self.assertLess(publish_refs, sync_pr)
        self.assertIn("gh pr create --base develop", content[sync_pr:])

    def test_manual_release_consumes_only_exact_versioned_installers(self) -> None:
        content = self.read(".github/workflows/build-windows.yml")
        self.assertIn("artifact_version=$artifactVersion", content)
        self.assertIn(
            'build/dist/MIB_Studio_Qt_Setup_v${{ steps.version.outputs.artifact_version }}.exe',
            content,
        )
        self.assertIn(
            'build/dist/MIB_Studio_Qt_Update_v${{ steps.version.outputs.artifact_version }}.exe',
            content,
        )

        release_start = content.index("name: Create GitHub Release")
        r2_start = content.index("name: Publish update package to Cloudflare R2")
        release_block = content[release_start:r2_start]
        r2_block = content[r2_start:]
        exact_setup = 'Get-Item -LiteralPath "build\\dist\\MIB_Studio_Qt_Setup_v$artifactVersion.exe"'
        exact_update = 'Get-Item -LiteralPath "build\\dist\\MIB_Studio_Qt_Update_v$artifactVersion.exe"'
        self.assertIn(exact_setup, release_block)
        self.assertIn(exact_update, release_block)
        self.assertIn(exact_update, r2_block)
        self.assertNotIn("Get-ChildItem", release_block)
        self.assertNotIn("Get-ChildItem", r2_block)

    def test_publishers_configure_and_validate_exact_binary_identity(self) -> None:
        for path in (
            ".github/workflows/build-windows.yml",
            ".github/workflows/release.yml",
            "release.ps1",
        ):
            with self.subTest(path=path):
                content = self.read(path)
                self.assertIn("-DMIB_RELEASE_VERSION_OVERRIDE=", content)
                self.assertIn("-DMIB_RELEASE_VERSION_FULL_OVERRIDE=", content)
                self.assertIn("mib-release-identity.txt", content)

        manual = self.read(".github/workflows/build-windows.yml")
        local = self.read("release.ps1")
        self.assertIn("resolve_desktop_release_version.py", manual)
        self.assertIn("resolve_desktop_release_version.py", local)
        self.assertLess(
            local.index("git fetch origin --tags"),
            local.index("resolve_desktop_release_version.py"),
        )

    def test_release_entrypoints_build_tests_before_packaging(self) -> None:
        manual = self.read(".github/workflows/build-windows.yml")
        manual_build = manual.index("name: CMake build")
        manual_tests = manual.index("name: Run tests")
        manual_package = manual.index("name: Build full installer")
        self.assertLess(manual_build, manual_tests)
        self.assertLess(manual_tests, manual_package)
        manual_build_block = manual[manual_build:manual_tests]
        self.assertIn("cmake --build build --config Release", manual_build_block)
        self.assertNotIn("--target mib_studio_qt", manual_build_block)

        local = self.read("release.ps1")
        local_build = local.index('cmake --build "$PSScriptRoot\\build" --config Release')
        local_tests = local.index('ctest --test-dir "$PSScriptRoot\\build"')
        local_package = local.index("# --- Step 4: Build installers ---")
        self.assertLess(local_build, local_tests)
        self.assertLess(local_tests, local_package)

        tagged = self.read(".github/workflows/release.yml")
        tagged_build = tagged.index("name: Build Release")
        tagged_tests = tagged.index("name: Run tests")
        tagged_sentry = tagged.index("name: Upload debug symbols + create Sentry release")
        tagged_package = tagged.index("name: Build full installer")
        self.assertLess(tagged_build, tagged_tests)
        self.assertLess(tagged_tests, tagged_sentry)
        self.assertLess(tagged_tests, tagged_package)

    def test_local_push_and_publish_fail_closed(self) -> None:
        content = self.read("release.ps1")
        self.assertIn("Run release.ps1 from the repository root", content)
        self.assertIn("OrdinalIgnoreCase.Equals", content)
        self.assertIn("Release requires a clean working tree", content)
        self.assertNotIn("Read-Host", content)
        self.assertIn("A pushed stable release must run from main", content)
        self.assertIn("$Push -and -not $Beta -and $currentBranch -ne 'main'", content)
        self.assertIn("git push --atomic origin", content)
        self.assertNotIn("WARNING: Branch push failed", content)
        self.assertNotIn("WARNING: GitHub Release creation failed", content)
        self.assertNotIn("WARNING: Cloudflare R2 publish failed", content)
        self.assertIn('"--version", $tagName.Substring(1)', content)
        self.assertIn("ERROR: GitHub Release creation failed", content)
        self.assertIn("ERROR: Cloudflare R2 publish failed", content)


if __name__ == "__main__":
    unittest.main()
