#!/usr/bin/env python3
"""Rehearsal hooks for the PZ7035 camera document (bridge ABI 38): apply a Run setting through a host Studio's real server path, then verify what the Run used.

  camera_document_cycle.py apply  [--fps 1000] [--exposure auto|US]   (BEFORE_DRIVER)  write <data>/camera/camera.json, check it, apply it over the WebSocket
  camera_document_cycle.py verify [--fps 1000] [--exposure auto|US]   (AFTER_DRIVER)   fetch_run_fps after the Run, and the HDF5 run snapshot's run_timing

On the board (--remote, any of the three; --url and --out instead of the REH_* environment) nothing is written locally: the document is made the way the UI makes it, through
the server: camera_document default -> edit frame_rate_hz / exposure_us -> create (or save, with the revision read first) as kind pz7035 under the board's <data>/camera/ ->
check_pz_camera_document -> apply_pz_camera_document. `restore --remote` does the same with 5000 / auto, to leave the instrument's data directory at the standing setting.
  camera_document_cycle.py apply   --remote --url URL --out DIR --fps 1000 [--exposure auto|US] [--acknowledge-discard-ring]
  camera_document_cycle.py verify  --remote --url URL --out DIR --fps 1000
  camera_document_cycle.py restore --remote --url URL --out DIR [--acknowledge-discard-ring]

URL, WORK, DATA and LABEL come from the REH_* environment run_rehearsal.sh sets. Evidence goes to $REH_WORK/camera-document-{apply,verify}.json. Exit 0 only when every
check holds. A host Studio has no Aravis, so what is checked is the Studio side: the document, the stored setting, the Run entry, the readiness gates and the snapshot;
the camera and the producer are the board's. Needs websockets (slot_driver.py's client) and h5py for the snapshot.
"""
import argparse
import json
import os
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import slot_driver as D  # noqa: E402


def expected(fps, exposure):
    """The timing the producer rule gives (include/backend/pz/PzRunTiming.h): HMAX 58 above clock / (116 x 241), else 116; auto starts the exposure at SHS 64 / 32."""
    clock = 74.25e6
    hmax = 58 if fps > clock / (116 * (96 + 145)) else 116
    vmax = int(clock // (hmax * fps))
    shs = 64 * 58 // hmax if exposure == "auto" else None
    return {"hmax": hmax, "vmax": vmax, "shs": shs}


def remote_document(st, check, ev, fps, exposure, name="camera.json"):
    """The UI's path through the server, writing nothing locally: default -> edit -> create/save (kind pz7035, the server's <data>/camera/). Returns (text, server path)."""
    def doc_call(action, path="", baseline="", text=""):
        return st.call("camera_document", {"action": action, "path": path, "kind": "pz7035", "baseline": baseline, "text": text})
    default = doc_call("default")
    ev["default"] = default
    check(isinstance(default, dict) and "error" not in default and "text" in default, "camera_document default: the bundled default document")
    doc = json.loads(default["text"])
    doc["run"]["frame_rate_hz"] = fps
    doc["run"]["exposure_us"] = exposure
    text = json.dumps(doc, indent=2) + "\n"
    existing = doc_call("read", name)
    ev["existing"] = {k: existing.get(k) for k in ("path", "revision", "error")} if isinstance(existing, dict) else existing
    if isinstance(existing, dict) and "error" not in existing:
        saved = doc_call("save", name, existing["revision"], text)
        verb = "save"
    else:
        saved = doc_call("create", name, "", text)
        verb = "create"
    ev["saved"] = {k: saved.get(k) for k in ("path", "revision", "error")} if isinstance(saved, dict) else saved
    check(isinstance(saved, dict) and "error" not in saved and saved.get("text") == text, f"camera_document {verb} {name} (kind pz7035): {saved.get('path') if isinstance(saved, dict) else saved}")
    return text, saved.get("path", "") if isinstance(saved, dict) else ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["apply", "verify", "restore"])
    ap.add_argument("--fps", type=int, default=int(os.environ.get("REH_FPS", "1000")))
    ap.add_argument("--exposure", default="auto")
    ap.add_argument("--remote", action="store_true", help="the board: do everything through the server, write nothing locally except the evidence file")
    ap.add_argument("--url", default=os.environ.get("REH_URL"))
    ap.add_argument("--out", default=os.environ.get("REH_WORK"), help="the evidence directory")
    ap.add_argument("--acknowledge-discard-ring", action="store_true", help="the apply re-enters Run: agree to discard a stopped Run's buffered frames")
    a = ap.parse_args()
    if a.mode == "restore":
        if not a.remote:
            ap.error("restore is the board's: use --remote")
        a.fps, a.exposure = 5000, "auto"
    if a.remote:
        if not (a.url and a.out):
            ap.error("--remote needs --url and --out")
        url, work, data, label = a.url, a.out, None, f"remote-{a.mode}"
        Path(work).mkdir(parents=True, exist_ok=True)
    else:
        url, work, data, label = (os.environ[k] for k in ("REH_URL", "REH_WORK", "REH_DATA", "REH_LABEL"))
    exposure = a.exposure if a.exposure == "auto" else float(a.exposure)
    want = expected(a.fps, exposure)
    fails = []

    def check(ok, what):
        print(("ok   " if ok else "FAIL ") + what)
        if not ok:
            fails.append(what)

    st = D.Studio(url, lambda m: print(m))
    ev = {"label": label, "fps": a.fps, "exposure": exposure, "expected": want}
    if a.mode in ("apply", "restore"):
        if not st.get("/healthz").get("initialized"):
            check(st.call("init", {"dataDir": ""}, wait=300) is True, "backend initialised")
        st.call("take_control")
        before = st.call("fetch_run_fps")
        ev["before"] = before
        if a.remote:
            text, path = remote_document(st, check, ev, a.fps, exposure)
            if not path:
                fails.append("no server path for the document")
                Path(work, f"camera-document-{a.mode}.json").write_text(json.dumps(ev, indent=1, default=str))
                sys.exit(1)
        else:
            check(before.get("available") and before.get("fps") == 5000.0 and before.get("exposure_mode") == "auto", "the default Run setting is 5000 fps / auto before the document")
            doc = {"schema": "pz7035-camera/1", "run": {"frame_rate_hz": a.fps, "exposure_us": exposure}}
            folder = Path(data) / "camera"
            folder.mkdir(parents=True, exist_ok=True)
            path = folder / "camera.json"
            path.write_text(json.dumps(doc, indent=2) + "\n")
            text, path = path.read_text(), str(path)
        chk = st.call("check_pz_camera_document", {"text": text})
        ev["check"] = chk
        check(chk.get("ok") is True and chk.get("hmax") == want["hmax"] and chk.get("vmax") == want["vmax"] and (want["shs"] is None or chk.get("shs") == want["shs"]),
              f"check_pz_camera_document: HMAX {want['hmax']} VMAX {want['vmax']} SHS {want['shs']} ({chk.get('exposure_us')} us)")
        # The server's confinement: a path outside <data>/camera/ is refused, and so is the desktop-only kind.
        outside = st.call("apply_pz_camera_document", {"path": "/etc/passwd", "acknowledgeDiscardRing": False})
        ev["outside"] = outside
        check(isinstance(outside, dict) and (outside.get("ok") is False or "error" in outside), "apply_pz_camera_document refuses a path outside <data>/camera/")
        kind = st.call("camera_document", {"action": "read", "path": path, "kind": "json", "baseline": "", "text": ""})
        ev["kind_json"] = kind
        check(isinstance(kind, dict) and "error" in kind, "camera_document refuses the desktop-only json kind on the server")
        res = st.call("apply_pz_camera_document", {"path": path, "acknowledgeDiscardRing": bool(a.acknowledge_discard_ring)})
        ev["apply"] = res
        check(isinstance(res, dict) and res.get("ok") is True, f"apply_pz_camera_document: {res.get('message') if isinstance(res, dict) else res}")
        after = st.call("fetch_run_fps")
        ev["after"] = after
        check(after.get("fps") == float(a.fps) and after.get("hmax") == want["hmax"] and after.get("vmax") == want["vmax"], f"the configured setting is {a.fps} fps (HMAX {want['hmax']}, VMAX {want['vmax']})")
        if after.get("in_run"):
            check(after.get("applied_fps") == float(a.fps) and after.get("pending") is False, f"in Run the setting was applied at once (applied_fps {after.get('applied_fps')})")
        else:
            check(after.get("applied_fps") is None, "outside Run nothing is applied yet (it applies at the Run entry)")
        check((after.get("qualified") is True) == (a.fps == 5000 and exposure == "auto"), "qualified only for 5000 fps / auto")
    else:
        run = st.call("fetch_run_fps")
        ev["run"] = run
        check(run.get("in_run") is True and run.get("applied_fps") == float(a.fps), f"the Run entry applied {a.fps} fps (applied_fps {run.get('applied_fps')})")
        check(run.get("pending") is False, "nothing pending: the camera runs the configured setting")
        if exposure == "auto":
            check(abs((run.get("applied_exposure_us") or 0) - run.get("exposure_us", -1)) < 1e-6, f"applied exposure equals the configured one ({run.get('applied_exposure_us')} us)")
        if a.remote:  # the HDF5 file is on the board
            ev["failures"] = fails
            Path(work, f"camera-document-{a.mode}.json").write_text(json.dumps(ev, indent=1, default=str))
            sys.exit(1 if fails else 0)
        import h5py
        files = sorted(Path(os.environ.get("REH_H5DIR", work)).rglob("slot693_*.h5"))
        ev["h5"] = [str(f) for f in files]
        check(len(files) == 1, f"one HDF5 experiment file ({len(files)})")
        if files:
            with h5py.File(files[0], "r") as h:
                snap = None
                def find(name, obj):
                    nonlocal snap
                    if snap is None and "run_snapshot_json" in obj.attrs:
                        snap = obj.attrs["run_snapshot_json"]
                    if snap is None and name.endswith("run_snapshot_json") and hasattr(obj, "shape"):
                        snap = obj[()]
                if "run_snapshot_json" in h.attrs:
                    snap = h.attrs["run_snapshot_json"]
                h.visititems(find)
                if isinstance(snap, bytes):
                    snap = snap.decode()
                if snap is not None and not isinstance(snap, str):
                    snap = snap.item().decode() if hasattr(snap, "item") else str(snap)
            rt = (json.loads(snap) if snap else {}).get("run_timing", {})
            ev["run_timing"] = rt
            check(rt.get("fps") == float(a.fps) and rt.get("hmax") == want["hmax"] and rt.get("vmax") == want["vmax"] and (want["shs"] is None or rt.get("shs") == want["shs"]),
                  f"the run snapshot records run_timing {rt}")
    ev["failures"] = fails
    Path(work, f"camera-document-{a.mode}.json").write_text(json.dumps(ev, indent=1, default=str))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
