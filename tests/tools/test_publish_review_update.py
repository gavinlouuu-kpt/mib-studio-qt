"""Unit tests for scripts/release/publish-review-update.py (YOFO Review's
Tauri-updater latest.json: per-channel keys, signatures, the SHA-256 pin)."""
import hashlib
import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path

_ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location("pubreview", _ROOT / "scripts" / "release" / "publish-review-update.py")
pub = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(pub)


class LatestJson(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        d = Path(self.tmp.name)
        self.app = d / "YOFO Review.app.tar.gz"
        self.app.write_bytes(b"mac bundle")
        self.exe = d / "YOFO Review_1.2.0_x64-setup.exe"
        self.exe.write_bytes(b"windows installer")

    def tearDown(self):
        self.tmp.cleanup()

    def test_both_platforms_stable(self):
        latest, uploads = pub.build_latest(
            version="1.2.0", channel="stable", base_url="https://updates.yofo.bio/",
            notes="https://example/notes", pub_date="2026-10-04T00:00:00Z",
            artifacts={"darwin-aarch64": (self.app, "SIGMAC\n"), "windows-x86_64": (self.exe, "SIGWIN")},
        )
        self.assertEqual(latest["version"], "1.2.0")
        mac = latest["platforms"]["darwin-aarch64"]
        self.assertEqual(mac["url"], "https://updates.yofo.bio/review-stable/YOFO_Review_v1.2.0_aarch64.app.tar.gz")
        self.assertEqual(mac["signature"], "SIGMAC")
        self.assertEqual(mac["sha256"], hashlib.sha256(b"mac bundle").hexdigest())
        win = latest["platforms"]["windows-x86_64"]
        self.assertEqual(win["url"], "https://updates.yofo.bio/review-stable/YOFO_Review_v1.2.0_x64-setup.exe")
        self.assertEqual(uploads["review-stable/YOFO_Review_v1.2.0_x64-setup.exe"], str(self.exe))
        json.dumps(latest)

    def test_channel_and_refusals(self):
        self.assertEqual(pub.channel_for("1.2.0-beta.abc1234"), "beta")
        self.assertEqual(pub.channel_for("1.2.0"), "stable")
        ok = {"windows-x86_64": (self.exe, "S")}
        with self.assertRaises(ValueError):
            pub.build_latest(version="v1.2", channel="stable", base_url="u", notes="", pub_date="", artifacts=ok)
        with self.assertRaises(ValueError):
            pub.build_latest(version="1.2.0", channel="nightly", base_url="u", notes="", pub_date="", artifacts=ok)
        with self.assertRaises(ValueError):
            pub.build_latest(version="1.2.0", channel="stable", base_url="u", notes="", pub_date="", artifacts={})
        with self.assertRaises(ValueError):
            pub.build_latest(version="1.2.0", channel="stable", base_url="u", notes="", pub_date="",
                             artifacts={"windows-x86_64": (self.exe, "  ")})
        with self.assertRaises(ValueError):
            pub.build_latest(version="1.2.0", channel="stable", base_url="u", notes="", pub_date="",
                             artifacts={"linux-x86_64": (self.exe, "S")})

    def test_dry_run_cli(self):
        sig = Path(self.tmp.name) / "win.sig"
        sig.write_text("SIG")
        out = Path(self.tmp.name) / "latest.json"
        rc = pub.main(["--version", "1.2.0-beta.abc1234", "--windows-installer", str(self.exe),
                       "--windows-sig", str(sig), "--dry-run", "--manifest-out", str(out)])
        self.assertEqual(rc, 0)
        latest = json.loads(out.read_text())
        self.assertIn("review-beta/", latest["platforms"]["windows-x86_64"]["url"])
        self.assertEqual(pub.main(["--version", "1.2.0", "--windows-installer", str(self.exe), "--dry-run"]), 2)


if __name__ == "__main__":
    unittest.main()
