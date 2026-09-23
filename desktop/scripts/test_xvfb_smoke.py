"""Regression: successful smoke must not leak the application or its children."""
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time
import unittest


def alive(pid):
    try:
        return Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()[0] != 'Z'
    except FileNotFoundError:
        return False


class SmokeCleanupTest(unittest.TestCase):
    def test_application_and_child_are_reaped_after_success(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            executable = root / 'fake-app'
            pids = root / 'owned-pids'
            executable.write_text('#!/usr/bin/env bash\nsleep 60 &\nprintf "%s\\n%s\\n" "$$" "$!" > "$SMOKE_TEST_PID_FILE"\nwait\n')
            executable.chmod(0o700)
            try:
                result = subprocess.run(['bash', str(Path(__file__).with_name('xvfb-smoke.sh')), str(executable), '1'],
                                        env={**os.environ, 'SMOKE_TEST_PID_FILE': str(pids)},
                                        capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                owned = [int(value) for value in pids.read_text().splitlines()]
                deadline = time.monotonic() + 3
                while any(alive(pid) for pid in owned) and time.monotonic() < deadline:
                    time.sleep(.05)
                self.assertFalse([pid for pid in owned if alive(pid)], 'Smoke leaked an application process')
            finally:
                if pids.exists():
                    for value in pids.read_text().splitlines():
                        try:
                            os.kill(int(value), signal.SIGKILL)
                        except ProcessLookupError:
                            pass


if __name__ == '__main__':
    unittest.main()
