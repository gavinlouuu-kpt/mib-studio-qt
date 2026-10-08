#!/usr/bin/env python3
"""The standing YOFO Studio unit for the PZ7035 (deploy/yofo-studio): the PL-ready guard reads
only DEVCFG INT_STS, the unit is loopback-only without a token, and install.sh verifies MD5SUMS
before touching the system."""
import os
import shutil
import stat
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEPLOY = ROOT / "deploy" / "yofo-studio"


def fake_devmem2(directory: Path, value: str, log: Path) -> None:
    script = directory / "devmem2"
    script.write_text(
        "#!/bin/sh\n"
        f"echo \"$@\" >> {log}\n"
        f"echo 'Read at address  0xF800700C (0xb6f0000c): {value}'\n"
    )
    script.chmod(script.stat().st_mode | stat.S_IEXEC)


def run_guard(value: str) -> tuple[int, list[str]]:
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        log = tmp_path / "calls.log"
        fake_devmem2(tmp_path, value, log)
        env = dict(os.environ, PATH=f"{tmp}:{os.environ['PATH']}")
        result = subprocess.run(["sh", str(DEPLOY / "pl-ready.sh"), "--once"], env=env, timeout=20)
        calls = log.read_text().splitlines() if log.exists() else []
        return result.returncode, calls


def make_bundle_fixture(tmp: Path) -> tuple[Path, dict]:
    """A fake pz7035 repo (tag pl-test), its PL build, firmware, boot set, producer, tools; and a staged package."""
    git = lambda *a, cwd=tmp: subprocess.run(["git", *a], cwd=cwd, check=True, capture_output=True, text=True).stdout.strip()
    repo = tmp / "pz7035"
    repo.mkdir()
    git("init", "-q", cwd=repo)
    git("-c", "user.name=t", "-c", "user.email=t@t", "commit", "--allow-empty", "-qm", "init", cwd=repo)
    git("tag", "pl-test", cwd=repo)
    build = repo / "build" / "pz_live_test"
    ps7 = build / "pz_live.gen" / "sources_1" / "bd" / "ps7" / "ip" / "ps7_ps_0"
    ps7.mkdir(parents=True)
    (build / "pz_live.bit").write_bytes(b"BIT")
    (ps7 / "ps7_init.tcl").write_text("# ps7\n")
    (build / "core.json").write_text(
        '{"build_id": "0123456789abcdef0123456789abcdef", "image": "pz_live_test", "abi": {"major": 1, "minor": 3}}')
    files = {
        "PZ_FIRMWARE": tmp / "live_server.elf", "PZ_CTI": tmp / "libpz7035_gentl.cti", "PZ_DTB": tmp / "x.dtb",
        "PZ_ROOTFS": tmp / "root.cpio.gz.u-boot", "PZ_PZPUMP": tmp / "pzpump",
    }
    for path in files.values():
        path.write_bytes(path.name.encode())
    tools = tmp / "tools"
    for tool in ("pzcell", "pzres"):
        (tools / tool).mkdir(parents=True)
        (tools / tool / tool).write_bytes(tool.encode())
    slot = tmp / "slot"
    slot.mkdir()
    (slot / "page.bin").write_bytes(b"p")
    (slot / "lut.bin").write_bytes(b"l")
    import hashlib
    env = {k: str(v) for k, v in files.items()}
    env.update(PZ7035_REPO=str(repo), PZ7035_REF="pl-test", PZ_LINUX_REPO=str(repo), PZ_TOOLS_DIR=str(tools),
               PZ_SLOT_DATA=str(slot), PZ_CTI_MD5=hashlib.md5(files["PZ_CTI"].read_bytes()).hexdigest())
    pkg = tmp / "pkg"
    pkg.mkdir()
    (pkg / "BUILD_INFO").write_text("yofo-studio standing package, commit abc12345\nbuilt: now\nexpects: nothing\n")
    for name in ("yofo-studio-server", "dist.tar", "yofo-studio.service", "pl-ready.sh"):
        (pkg / name).write_text(name)
    shutil.copy(DEPLOY / "install.sh", pkg / "install.sh")
    return pkg, env


def check_bundle(check) -> None:
    script = ROOT / "scripts" / "yofo" / "bundle_assemble.sh"
    with tempfile.TemporaryDirectory() as tmp:
        pkg, env = make_bundle_fixture(Path(tmp))
        run = subprocess.run(["bash", str(script), str(pkg)], env=dict(os.environ, **env), capture_output=True, text=True)
        check(run.returncode == 0, f"bundle assembles: {run.stderr}")
        listed = {line.split(None, 1)[1] for line in (pkg / "MD5SUMS").read_text().splitlines()}
        wanted = {"host/pl/pz_live.bit", "host/pl/ps7_init.tcl", "host/firmware/live_server.elf", "host/linux/x.dtb",
                  "host/linux/root.cpio.gz.u-boot", "host/linux/bootargs", "core.json", "producer/libpz7035_gentl.cti",
                  "tools/pzcell", "tools/pzres", "tools/pzpump", "tools/page.bin", "tools/lut.bin", "yofo-studio-server",
                  "install.sh", "BUILD_INFO"}
        check(wanted <= listed, f"MD5SUMS lists every file of the bundle (missing {sorted(wanted - listed)})")
        check(subprocess.run(["md5sum", "-c", "--quiet", "MD5SUMS"], cwd=pkg).returncode == 0, "MD5SUMS verifies")
        first = (pkg / "BUILD_INFO").read_text().splitlines()[0]
        check("mib-studio-qt abc12345" in first and "pz7035-imx426" in first and "0123456789"[:8] in first and "ABI 1.3" in first,
              f"one BUILD_INFO line names both commits, the PL BUILD_ID and the ABI: {first}")
        # The board gets the board side only: install.sh checks everything outside host/.
        shutil.rmtree(pkg / "host")
        board = subprocess.run("grep -v '  host/' MD5SUMS | md5sum -c --quiet", shell=True, cwd=pkg)
        check(board.returncode == 0, "the board-side check passes without the host/ part")
        # A tagged ref is a release; a bare commit is marked as a pre-qualification build.
        check("PRE-QUALIFICATION" not in first, "a tagged ref is not marked pre-qualification")
        commit = subprocess.run(["git", "rev-parse", "pl-test"], cwd=env["PZ7035_REPO"], capture_output=True, text=True).stdout.strip()
        (pkg / "BUILD_INFO").write_text("yofo-studio standing package, commit abc12345\nbuilt: now\n")  # a fresh staged package
        pre = subprocess.run(["bash", str(script), str(pkg)], env={**os.environ, **env, "PZ7035_REF": commit, "PZ_PL_IMAGE": "test"},
                             capture_output=True, text=True)
        check(pre.returncode == 0 and "PRE-QUALIFICATION, untagged pz7035" in (pkg / "BUILD_INFO").read_text().splitlines()[0],
              f"an untagged commit is marked pre-qualification: {pre.stderr}")
        # A core.json of another image is refused.
        (Path(env["PZ7035_REPO"]) / "build" / "pz_live_test" / "core.json").write_text(
            '{"build_id": "ff", "image": "pz_live_other", "abi": {"major": 1, "minor": 3}}')
        wrong = subprocess.run(["bash", str(script), str(pkg)], env=dict(os.environ, **env), capture_output=True, text=True)
        check(wrong.returncode != 0 and "pz_live_other" in wrong.stderr, "a core.json for another image is refused")


def main() -> int:
    if sys.platform == "win32":  # board deployment scripts (sh, devmem2, bash, md5sum): the Linux build host checks them
        print("yofo standing package: skipped on Windows")
        return 0
    failures = []

    def check(condition: bool, message: str) -> None:
        if not condition:
            failures.append(message)

    # The blank PL the 2026-10-08 cold start read (0x00020000): refuse. The loaded PL (0x5802000F): go.
    code, calls = run_guard("0x00020000")
    check(code == 1, f"blank PL must not pass the guard (exit {code})")
    code, calls = run_guard("0x5802000F")
    check(code == 0, f"loaded PL must pass the guard (exit {code})")
    check(calls and all(c.split()[0].lower() == "0xf800700c" and c.split()[-1] == "w" for c in calls),
          f"the guard reads only DEVCFG INT_STS: {calls}")

    unit = (DEPLOY / "yofo-studio.service").read_text()
    check("ExecStartPre=/usr/libexec/yofo-studio/pl-ready" in unit, "unit waits for the PL")
    check("--listen 127.0.0.1:8427" in unit and "--no-token" in unit, "unit is loopback-only without a token")
    check("--token-file" not in unit, "unit does not need a token file")
    check("0.0.0.0" not in unit, "unit never listens on all interfaces")
    check("--resource-dir /usr/share/yofo-studio" in unit, "unit passes the resource dir (E-modulus LUT, G6)")
    check("MIB_CAMERA_MODE=aravis" in unit and "GENICAM_GENTL32_PATH=/usr/lib/genicam" in unit,
          "unit selects the Aravis/GenTL producer")

    install = (DEPLOY / "install.sh").read_text()
    check(install.index("md5sum -c") < install.index("install -m 0755 yofo-studio-server"),
          "install.sh verifies MD5SUMS before installing anything")
    check("/etc/yofo-studio/token" not in install.replace("# ", ""), "install.sh does not require a token")
    check("/usr/share/yofo-studio/BUILD_INFO" in install, "install.sh leaves BUILD_INFO next to the UI for /diagnostics")
    check("/usr/share/yofo-studio/resources/isoelastic_curve" in install, "install.sh installs the LUT under the resource dir")
    check((ROOT / "resources" / "isoelastic_curve" / "scaled_isoelastic_data_LUT_6.16-4.24.txt").is_file(), "the LUT the server loads exists in the repo")
    package = (ROOT / "scripts" / "yofo" / "package_studio.sh").read_text()
    check("resources/isoelastic_curve" in package and "resources/isoelastic_curve/*" in package, "the package ships the LUT and lists it in MD5SUMS")

    check_bundle(check)

    for failure in failures:
        print("FAIL:", failure, file=sys.stderr)
    print("yofo standing package: ok" if not failures else f"{len(failures)} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
