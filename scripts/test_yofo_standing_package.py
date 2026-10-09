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


FAKE_COMMANDS = {
    # exit 0 when the fake partition is "mounted" (a marker file the fake mount creates)
    "mountpoint": "#!/bin/sh\n[ -e \"$FAKE_MOUNTED\" ]\n",
    # a partition with the label exists when FAKE_LABEL is set
    "blkid": "#!/bin/sh\n[ -n \"${FAKE_LABEL:-}\" ]\n",
    # mount -o OPTS -L LABEL DIR: records the call, then "mounts": the partition's files appear in DIR
    "mount": (
        "#!/bin/sh\n"
        "echo \"mount $*\" >> \"$FAKE_LOG\"\n"
        "[ -z \"${FAKE_MOUNT_FAILS:-}\" ] || exit 32\n"
        "for last; do :; done\n"
        "touch \"$FAKE_MOUNTED\"\n"
        "mkdir -p \"$last/lost+found\"\n"
        "[ -d \"${FAKE_PARTITION:-/nonexistent}\" ] && cp -a \"$FAKE_PARTITION\"/. \"$last\"/\n"
        "exit 0\n"
    ),
    "systemctl": (
        "#!/bin/sh\necho \"systemctl $*\" >> \"$FAKE_LOG\"\n"
        "if [ \"$1\" = is-active ]; then [ -n \"${FAKE_UNIT_ACTIVE:-}\" ]; exit $?; fi\n"
        "exit 0\n"
    ),
}


def run_mount_data(tmp: Path, *, label=True, seed=None, partition=None, unit_active=False, mounted=False, mount_fails=False):
    """Run deploy/yofo-studio/mount-data.sh against fake mount/blkid/mountpoint/systemctl; returns (code, out, dir, log)."""
    bindir = tmp / "bin"
    bindir.mkdir(exist_ok=True)
    for name, body in FAKE_COMMANDS.items():
        path = bindir / name
        path.write_text(body)
        path.chmod(0o755)
    data = tmp / "data"
    shutil.rmtree(data, ignore_errors=True)
    data.mkdir()
    for name, text in (seed or {}).items():
        (data / name).write_text(text)
    part = tmp / "partition"
    shutil.rmtree(part, ignore_errors=True)
    if partition is not None:
        part.mkdir()
        for name, text in partition.items():
            (part / name).write_text(text)
    log = tmp / "log"
    log.write_text("")
    marker = tmp / "mounted"
    marker.unlink(missing_ok=True)
    if mounted:
        marker.touch()
    env = dict(os.environ, PATH=f"{bindir}:{os.environ['PATH']}", YOFO_DATA_DIR=str(data), FAKE_LOG=str(log), FAKE_MOUNTED=str(marker),
               FAKE_PARTITION=str(part))
    if label:
        env["FAKE_LABEL"] = "1"
    if unit_active:
        env["FAKE_UNIT_ACTIVE"] = "1"
    if mount_fails:
        env["FAKE_MOUNT_FAILS"] = "1"
    run = subprocess.run(["sh", str(DEPLOY / "mount-data.sh")], env=env, capture_output=True, text=True)
    return run.returncode, run.stdout + run.stderr, data, log.read_text().splitlines()


def check_mount_data(check) -> None:
    if sys.platform == "win32":
        return
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        # no partition with the label: the RAM directory stays as it is, nothing is mounted, and it says so
        code, out, data, log = run_mount_data(tmp, label=False, seed={"keep.txt": "ram"})
        check(code == 0 and not any(l.startswith("mount ") for l in log) and (data / "keep.txt").read_text() == "ram",
              f"no label: the RAM directory is left alone ({code}, {log})")
        check("volatile" in out, f"no label: the script says the directory is volatile: {out!r}")
        # already a mount point: nothing to do
        code, out, data, log = run_mount_data(tmp, mounted=True, seed={"keep.txt": "ram"})
        check(code == 0 and log == [] and (data / "keep.txt").exists(), f"already mounted: untouched ({log})")
        # a fresh (empty) partition is seeded from the RAM directory, and the options are the agreed ones
        code, out, data, log = run_mount_data(tmp, seed={"experiment.h5": "run", "instrument_run_window.json": "{}"}, partition={})
        check(code == 0 and any("mount -o noatime,commit=30 -L yofo-data" in l for l in log), f"mount options and label: {log}")
        check((data / "experiment.h5").read_text() == "run" and (data / "instrument_run_window.json").exists(),
              "an empty partition is seeded with the RAM directory's files")
        check(not list(tmp.glob("data.seed.*")), "the seed copy is removed")
        # a partition that already has data keeps it; the RAM files go into a from-ram folder
        code, out, data, log = run_mount_data(tmp, seed={"ram.h5": "ram"}, partition={"old.h5": "old"})
        folders = list(data.glob("from-ram-*"))
        check(code == 0 and (data / "old.h5").read_text() == "old" and not (data / "ram.h5").exists() and len(folders) == 1
              and (folders[0] / "ram.h5").read_text() == "ram", "a partition with data keeps it; the RAM files are kept beside it")
        # an empty RAM directory and an empty partition: just mounted
        code, out, data, log = run_mount_data(tmp, partition={})
        check(code == 0 and any(l.startswith("mount ") for l in log) and not list(data.glob("from-ram-*")), "nothing to seed")
        # the unit is stopped around the mount and started again
        code, out, data, log = run_mount_data(tmp, unit_active=True, seed={"a": "1"}, partition={})
        order = [l for l in log if l.startswith(("systemctl stop", "mount ", "systemctl start"))]
        check([o.split()[0] + " " + o.split()[1] for o in order] == ["systemctl stop", "mount -o", "systemctl start"],
              f"the running unit is stopped, the mount happens, the unit starts again: {order}")
        # a failed mount puts the RAM files back, exits non-zero and restarts the unit
        code, out, data, log = run_mount_data(tmp, unit_active=True, seed={"a": "1"}, partition={}, mount_fails=True)
        check(code != 0 and (data / "a").read_text() == "1" and not list(tmp.glob("data.seed.*")) and log[-1] == "systemctl start yofo-studio",
              f"failed mount: RAM files restored, unit restarted ({code}, {log})")


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
    check("WorkingDirectory=/var/lib/yofo-studio" in unit, "relative paths resolve under the data dir (G8)")
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
    check("mount-data.sh" in install and install.index("mount-data.sh") < install.index("install -m 0755 yofo-studio-server"), "install.sh mounts the data partition before it installs the server (and so before the unit starts)")
    check("/usr/share/yofo-studio/BUILD_INFO" in install, "install.sh leaves BUILD_INFO next to the UI for /diagnostics")
    check("/usr/share/yofo-studio/resources/isoelastic_curve" in install, "install.sh installs the LUT under the resource dir")
    check((ROOT / "resources" / "isoelastic_curve" / "scaled_isoelastic_data_LUT_6.16-4.24.txt").is_file(), "the LUT the server loads exists in the repo")
    package = (ROOT / "scripts" / "yofo" / "package_studio.sh").read_text()
    check("resources/isoelastic_curve" in package and "resources/isoelastic_curve/*" in package, "the package ships the LUT and lists it in MD5SUMS")

    check_bundle(check)
    check_mount_data(check)

    for failure in failures:
        print("FAIL:", failure, file=sys.stderr)
    print("yofo standing package: ok" if not failures else f"{len(failures)} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
