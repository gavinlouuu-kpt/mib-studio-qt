"""Unit tests for publish-update.py's merge_index (per-channel version index)."""
import importlib.util
import pathlib
import sys

_ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(_ROOT))  # so publish-update.py can import scripts.s3_upload
_spec = importlib.util.spec_from_file_location("pub", _ROOT / "scripts" / "release" / "publish-update.py")
pub = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(pub)


def entry(v):
    return {
        "version": v,
        "installer_url": f"https://updates.yofo.bio/stable/u_{v}.exe",
        "installer_sha256": "s",
        "installer_size_bytes": 1,
        "release_notes_url": "n",
        "published_utc": "t",
    }


def test_insert_into_empty():
    out = pub.merge_index({}, entry("1.0.4"), channel="stable")
    assert out["schema_version"] == 1
    assert out["channel"] == "stable"
    assert [v["version"] for v in out["versions"]] == ["1.0.4"]


def test_dedupe_and_order():
    idx = pub.merge_index({}, entry("1.0.3"), channel="stable")
    idx = pub.merge_index(idx, entry("1.0.4"), channel="stable")
    idx = pub.merge_index(idx, entry("1.0.3"), channel="stable")  # duplicate version
    versions = [v["version"] for v in idx["versions"]]
    assert versions == ["1.0.4", "1.0.3"], versions


def test_beta_orders_below_its_release():
    idx = pub.merge_index({}, entry("1.0.4-beta.1"), channel="beta")
    idx = pub.merge_index(idx, entry("1.0.4-beta.2"), channel="beta")
    idx = pub.merge_index(idx, entry("1.0.4"), channel="beta")
    versions = [v["version"] for v in idx["versions"]]
    assert versions == ["1.0.4", "1.0.4-beta.2", "1.0.4-beta.1"], versions


def test_successive_sha_betas_order_newest_publication_first():
    older = entry("1.0.6-beta.abc123")
    older["published_utc"] = "2026-07-14T10:00:00Z"
    newer = entry("1.0.6-beta.def456")
    newer["published_utc"] = "2026-07-14T11:00:00Z"
    idx = pub.merge_index({}, older, channel="beta")
    idx = pub.merge_index(idx, newer, channel="beta")
    assert [v["version"] for v in idx["versions"]] == [
        "1.0.6-beta.def456",
        "1.0.6-beta.abc123",
    ]


def test_replace_keeps_latest_fields():
    idx = pub.merge_index({}, entry("1.0.4"), channel="stable")
    updated = entry("1.0.4")
    updated["installer_sha256"] = "newhash"
    idx = pub.merge_index(idx, updated, channel="stable")
    assert len(idx["versions"]) == 1
    assert idx["versions"][0]["installer_sha256"] == "newhash"


def test_resolve_index_update_grows_from_existing():
    # The bug: each release replaced the index instead of growing it. With the
    # existing index readable, the new version must be ADDED, keeping the old one.
    existing = {"schema_version": 1, "channel": "stable", "versions": [entry("1.0.4")]}
    out = pub.resolve_index_update(existing, True, entry("1.0.5"), "stable")
    assert out is not None
    assert [v["version"] for v in out["versions"]] == ["1.0.5", "1.0.4"], out


def test_resolve_index_update_first_time():
    out = pub.resolve_index_update({}, True, entry("1.0.5"), "stable")
    assert [v["version"] for v in out["versions"]] == ["1.0.5"]


def test_resolve_index_update_skips_on_read_failure():
    # read_ok=False -> return None so the caller does NOT overwrite a good index.
    out = pub.resolve_index_update(None, False, entry("1.0.5"), "stable")
    assert out is None


def test_beta_artifact_keys_keep_full_release_identity():
    installer = pathlib.Path("MIB_Studio_Qt_Update_v1.0.6.exe")
    first = pub.published_artifact_name(installer, "1.0.6-beta.abc123")
    second = pub.published_artifact_name(installer, "1.0.6-beta.def456")
    assert first == "MIB_Studio_Qt_Update_v1.0.6-beta.abc123.exe"
    assert second == "MIB_Studio_Qt_Update_v1.0.6-beta.def456.exe"
    assert first != second


def test_explicit_version_must_match_numeric_installer_version():
    installer = pathlib.Path("MIB_Studio_Qt_Update_v1.0.6.exe")
    try:
        pub.published_artifact_name(installer, "1.0.7-beta.abc123")
    except ValueError as exc:
        assert "numeric version" in str(exc)
    else:
        raise AssertionError("mismatched release/installer identity was accepted")


if __name__ == "__main__":
    test_insert_into_empty()
    test_dedupe_and_order()
    test_beta_orders_below_its_release()
    test_successive_sha_betas_order_newest_publication_first()
    test_replace_keeps_latest_fields()
    test_resolve_index_update_grows_from_existing()
    test_resolve_index_update_first_time()
    test_resolve_index_update_skips_on_read_failure()
    test_beta_artifact_keys_keep_full_release_identity()
    test_explicit_version_must_match_numeric_installer_version()
    print("ok")
