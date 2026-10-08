#!/usr/bin/env python3
"""The standing YOFO Studio unit for the PZ7035 (deploy/yofo-studio): the PL-ready guard reads
only DEVCFG INT_STS, the unit is loopback-only without a token, and install.sh verifies MD5SUMS
before touching the system."""
import os
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


def main() -> int:
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
    check("MIB_CAMERA_MODE=aravis" in unit and "GENICAM_GENTL32_PATH=/usr/lib/genicam" in unit,
          "unit selects the Aravis/GenTL producer")

    install = (DEPLOY / "install.sh").read_text()
    check(install.index("md5sum -c MD5SUMS") < install.index("install -m 0755 yofo-studio-server"),
          "install.sh verifies MD5SUMS before installing anything")
    check("/etc/yofo-studio/token" not in install.replace("# ", ""), "install.sh does not require a token")

    for failure in failures:
        print("FAIL:", failure, file=sys.stderr)
    print("yofo standing package: ok" if not failures else f"{len(failures)} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
