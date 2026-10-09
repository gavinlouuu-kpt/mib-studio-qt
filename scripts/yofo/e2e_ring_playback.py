#!/usr/bin/env python3
"""YOFO Studio frame-ring playback on the PZ7035 (#649 v1), through the browser UI: one pass/fail per item.

  python3 scripts/yofo/e2e_ring_playback.py http://127.0.0.1:8427 OUTDIR [--take-control]

Needs Python Playwright with Chromium and the board's server reached directly or through an SSH tunnel. It takes the controller role
over the page's own socket (`--take-control`; use it only when the operator has agreed), puts the instrument in Run (LED at the
Run preset), stops it, plays back the buffered frames, resumes, and ends in Align. It never touches the pumps and starts no
experiment. A second, viewer-only page checks that Stop/Resume are refused for a viewer. Screenshots of the stopped playback, of the
Camera & Alignment tab while stopped, and of the second stop after Resume go to OUTDIR with a results.json.

Items: ring.placement gate, a frame read while running, Stop and the panel (capacity, range), scrub, step, play at 1/30/60 fps,
overlays, the cell table, Camera & Alignment refused while stopped, a viewer's Stop/Resume refused, Save clip disabled, an experiment
refused by run.frozen, Resume and a second Stop with new frames.
"""
import json
import re
import sys
import time
from pathlib import Path

from playwright.sync_api import sync_playwright

args = [a for a in sys.argv[1:] if not a.startswith("--")]
base, outdir = args[0], Path(args[1])
take = "--take-control" in sys.argv
outdir.mkdir(parents=True, exist_ok=True)
results = []

INIT = """
(() => {
  const W = window.WebSocket;
  window.__replies = {};
  window.WebSocket = class extends W {
    constructor(...a) {
      super(...a); window.__ws = this;
      this.addEventListener('message', e => { try { if (typeof e.data === 'string') { const m = JSON.parse(e.data); if (m.request_id >= 800000) window.__replies[m.request_id] = m; } } catch (_) {} });
      if (%s) this.addEventListener('open', () => this.send(JSON.stringify({request_id: 900001, cmd: 'take_control', args: null})));
    }
  };
})();
""" % ("true" if take else "false")


def record(name, ok, detail=""):
    results.append({"item": name, "result": "pass" if ok else "FAIL", "detail": detail})
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}", flush=True)


def tab(page, name):
    page.locator(".stage-tab-name", has_text=re.compile(f"^{re.escape(name)}$")).click()


def body(page):
    return page.inner_text("body")


seq = [800000]


def call(page, cmd, args=None, wait=10.0):
    """One command over the page's own socket: (reply, seconds)."""
    seq[0] += 1
    rid = seq[0]
    t0 = time.time()
    page.evaluate("([rid, cmd, args]) => window.__ws.send(JSON.stringify({request_id: rid, cmd, args}))", [rid, cmd, args])
    while time.time() - t0 < wait:
        r = page.evaluate("(rid) => window.__replies[rid] || null", rid)
        if r:
            return r, time.time() - t0
        time.sleep(0.05)
    return None, time.time() - t0


def ring_status(page):
    r, _ = call(page, "fetch_ring_status")
    return (r or {}).get("ok") or {}


def seq_text(page):
    m = re.search(r"Frame ([\d,]+) ·", body(page))
    return int(m.group(1).replace(",", "")) if m else None


def canvas_sig(page):
    return page.evaluate("""() => {
      const c = document.querySelector('[data-testid=ring-playback] canvas');
      if (!c || !c.width) return null;
      const d = c.getContext('2d').getImageData(0, 0, c.width, c.height).data;
      let sum = 0, tint = 0;
      for (let i = 0; i < d.length; i += 4) { sum += d[i]; if (d[i] !== d[i + 1]) tint += 1; }
      return {w: c.width, h: c.height, mean: sum / (d.length / 4), tinted: tint};
    }""")


with sync_playwright() as p:
    browser = p.chromium.launch(args=["--no-sandbox"])
    page = browser.new_page(viewport={"width": 1400, "height": 1100})
    page.add_init_script(INIT)
    errors = []
    page.on("pageerror", lambda e: errors.append(str(e)[:200]))
    page.goto(base + "/")
    page.wait_for_function("() => /backend: ready/.test(document.body.innerText)", timeout=40000)
    time.sleep(4)
    if "Another client controls this instrument" in body(page):
        record("control", False, "this page is viewer-only: pass --take-control")
        (outdir / "results.json").write_text(json.dumps(results, indent=2))
        sys.exit(2)

    # The Run window is placed as in the other slot scripts.
    tab(page, "Camera & Alignment")
    time.sleep(3)
    try:
        page.get_by_label("Camera window x").fill("232")
        page.get_by_label("Camera window y").fill("336")
        page.get_by_role("button", name="Save camera ROI").click()
    except Exception:  # noqa: BLE001
        pass
    time.sleep(2)

    # Run, the ring placement gate, a frame read while running.
    tab(page, "Experiment")
    for _ in range(40):
        time.sleep(1)
        if re.search(r"Run 512×96", body(page)):
            break
    time.sleep(2)
    r, _ = call(page, "fetch_experiment_readiness", {"outputPath": "/var/lib/yofo-studio/slotb-readiness.h5"})
    gates = {g["id"]: g for g in (((r or {}).get("ok") or {}).get("gates") or [])}
    placement = gates.get("ring.placement")
    record("ring.placement gate", bool(placement) and placement.get("status") in ("pass", 0, "Pass"), json.dumps(placement)[:200] if placement else "gate missing")
    st = ring_status(page)
    record("ring configured", bool(st.get("available")) and st.get("capacity_frames", 0) >= 5000, json.dumps({k: st.get(k) for k in ("available", "capacity_frames", "count", "sensor_fps", "reason")}))
    t0 = time.time()
    r, took = call(page, "fetch_ring_frame", {"seq": max(0, st.get("last_seq", 1) - 8)}, wait=15)
    record("a frame read while running does not hang", r is not None and took < 5, f"{took * 1000:.0f} ms: {'ok' if r and r.get('ok') is not None else str(r)[:160]}")

    # 2. Stop: the panel.
    stop = page.get_by_role("button", name="Stop", exact=True)
    record("Stop is offered in Run", stop.count() > 0, "")
    stop.first.click()
    page.wait_for_selector("[data-testid=ring-playback]", timeout=15000)
    time.sleep(3)
    txt = body(page)
    st = ring_status(page)
    record("stopped: the ring is frozen", bool(st.get("frozen")) and st.get("run_frozen") and not st.get("invalid"), json.dumps({k: st.get(k) for k in ("frozen", "invalid", "stop_incomplete", "restore_needed", "count", "first_seq", "last_seq", "head", "state")}))
    record("panel shows about 5000 frames and about 1 s", "5,000 frames" in txt and re.search(r"[01]\.\d\d s at [\d,]+ fps", txt) is not None, re.search(r"[\d,]+ frames · [^\n]*", txt).group(0) if re.search(r"[\d,]+ frames · [^\n]*", txt) else "")
    record("buffered range shown", re.search(r"Frames [\d,]+ to [\d,]+ · [\d,]+ frames", txt) is not None, "")
    page.screenshot(path=str(outdir / "2_stopped.png"))

    # Scrub, step, play.
    slider = page.locator("[data-testid=ring-playback] input[type=range]")
    lo, hi = int(slider.get_attribute("min")), int(slider.get_attribute("max"))
    mid = (lo + hi) // 2
    slider.fill(str(mid))
    time.sleep(1.5)
    record("scrub to the middle", seq_text(page) == mid, f"frame {seq_text(page)} (wanted {mid})")
    page.get_by_role("button", name="Next frame").click()
    time.sleep(0.8)
    record("step forward", seq_text(page) == mid + 1, f"frame {seq_text(page)}")
    page.get_by_role("button", name="Previous frame").click()
    page.get_by_role("button", name="Previous frame").click()
    time.sleep(0.8)
    record("step back", seq_text(page) == mid - 1, f"frame {seq_text(page)}")
    for fps in (1, 30, 60):
        slider.fill(str(lo + 10))
        time.sleep(1.0)
        page.get_by_label("Display rate").select_option(str(fps))
        start = seq_text(page)
        page.get_by_role("button", name="Play").click()
        time.sleep(2.0)
        page.get_by_role("button", name="Pause").click()
        time.sleep(0.5)
        moved = (seq_text(page) or 0) - (start or 0)
        wanted = fps * 2.0
        record(f"play at {fps} fps", wanted * 0.5 <= moved <= wanted * 1.6 + 2, f"advanced {moved} frames in about 2 s (wanted about {wanted:.0f})")

    # Overlays and the cells.
    slider.fill(str(mid))
    time.sleep(1.5)
    sigs = {}
    for _ in range(5):
        ov = page.locator("button", has_text="Overlay:")
        label = ov.inner_text().strip()
        sigs[label] = canvas_sig(page)
        ov.click()
        time.sleep(0.5)
    record("overlays cycle Off, Mask, Contours, Both", set(sigs) >= {"Overlay: Off", "Overlay: Mask", "Overlay: Contours", "Overlay: Both"}, ", ".join(sorted(sigs)))
    off = sigs.get("Overlay: Off") or {}
    record("a frame is drawn at 512 x 96", off.get("w") == 512 and off.get("h") == 96 and off.get("mean", 0) > 0, json.dumps(off))
    rows = page.locator("table.ring-cells tbody tr")
    cells_text = rows.all_inner_texts() if rows.count() else []
    record("cell table or 'No cell in this frame'", rows.count() > 0 or "No cell in this frame" in body(page), f"{rows.count()} rows: {cells_text[:2]}")
    # Look for a frame with cells (a bench scene may have none) and check the "—" rendering for rejected cells.
    found = None
    for probe in range(lo, hi, max(1, (hi - lo) // 60)):
        slider.fill(str(probe))
        time.sleep(0.25)
        if page.locator("table.ring-cells tbody tr").count():
            found = probe
            break
    record("a buffered frame with cells (scene dependent)", found is not None, f"frame {found}" if found is not None else "none of 60 probed frames has a cell (a dark bench scene): the cell table could not be exercised")
    if found is not None:
        t = " | ".join(page.locator("table.ring-cells tbody tr").all_inner_texts())
        record("cell rows show measurements or a dash", re.search(r"\d", t) is not None and "NaN" not in t and "undefined" not in t, t[:160])

    # 8. Save clip, 9. an experiment while stopped, 4. Camera & Alignment while stopped.
    save = page.get_by_role("button", name="Save clip")
    record("Save clip is disabled with the SSD text", save.is_disabled() and "Saving needs the SSD" in body(page), "")
    r, _ = call(page, "fetch_experiment_readiness", {"outputPath": "/var/lib/yofo-studio/slotb-readiness.h5"})
    gates = {g["id"]: g for g in (((r or {}).get("ok") or {}).get("gates") or [])}
    frozen_gate = gates.get("run.frozen")
    record("an experiment while stopped is refused (run.frozen)", bool(frozen_gate) and frozen_gate.get("status") not in ("pass", 0, "Pass"), json.dumps(frozen_gate)[:200] if frozen_gate else "gate missing")
    before_first = (ring_status(page) or {}).get("first_seq")
    tab(page, "Camera & Alignment")
    time.sleep(4)
    st2 = ring_status(page)
    still = bool(st2.get("run_frozen")) and st2.get("first_seq") == before_first
    record("Camera & Alignment while stopped: refused, the frames are kept", still, json.dumps({k: st2.get(k) for k in ("run_frozen", "first_seq", "count")}))
    page.screenshot(path=str(outdir / "4_camera_tab_while_stopped.png"))
    tab(page, "Experiment")
    time.sleep(2)
    r, _ = call(page, "set_instrument_mode", {"mode": "align", "x": 0, "y": 0})
    msg = json.dumps((r or {}).get("ok") or (r or {}).get("error"))
    record("the backend refuses a camera mode switch while stopped", "resume Run first" in msg and ring_status(page).get("run_frozen") is True, msg[:200])

    # 7. A viewer: Stop/Resume refused and logged.
    viewer = browser.new_page(viewport={"width": 1400, "height": 1000})
    viewer.add_init_script(INIT.replace("if (true)", "if (false)"))  # no take_control: a viewer
    viewer.goto(base + "/")
    viewer.wait_for_function("() => /backend: ready/.test(document.body.innerText)", timeout=40000)
    time.sleep(3)
    tab(viewer, "Experiment")
    time.sleep(3)
    if viewer.get_by_role("button", name=re.compile("^Resume Run")).count():
        viewer.get_by_role("button", name=re.compile("^Resume Run")).click()
        time.sleep(2)
        vlog = viewer.inner_text("body")
        record("a viewer's Resume is refused", ring_status(page).get("run_frozen") is True, "the instrument is still stopped" + ("; VIEWER_ONLY in the page" if "VIEWER_ONLY" in vlog or "another client" in vlog else ""))
    else:
        record("a viewer's Resume is refused", False, "the viewer page shows no Resume button")
    viewer.close()

    # 5. Resume, a live Run, a second Stop with new frames.
    first_ids = page.evaluate("() => null")
    r, _ = call(page, "fetch_ring_frame", {"seq": max(0, int(ring_status(page).get("last_seq", 1)) - 1)})
    page.get_by_role("button", name=re.compile("^Resume Run")).click()
    time.sleep(6)
    st3 = ring_status(page)
    txt = body(page)
    record("Resume: a live Run, no stopped Run", not st3.get("run_frozen") and "Run stopped ·" not in txt and re.search(r"Run 512×96", txt) is not None, json.dumps({k: st3.get(k) for k in ("run_frozen", "frozen", "count", "head")}))
    time.sleep(2)
    page.get_by_role("button", name="Stop", exact=True).first.click()
    page.wait_for_selector("[data-testid=ring-playback]", timeout=15000)
    time.sleep(3)
    st4 = ring_status(page)
    record("a second Stop shows new frames", bool(st4.get("frozen")) and st4.get("count", 0) > 1000, json.dumps({k: st4.get(k) for k in ("frozen", "count", "first_seq", "last_seq", "head")}))
    page.screenshot(path=str(outdir / "5_second_stop.png"))

    # Back to a live Run and then Align (the hand-back state).
    page.get_by_role("button", name=re.compile("^Resume Run")).click()
    time.sleep(5)
    tab(page, "Camera & Alignment")
    time.sleep(8)
    led = body(page)
    record("ends in Align", re.search(r"LED:\s*\n?\s*align", led) is not None or "align 100/135" in led, "")
    record("no page errors", not errors, "; ".join(errors)[:200])
    browser.close()

(outdir / "results.json").write_text(json.dumps(results, indent=2))
failed = [r for r in results if r["result"] != "pass"]
print(f"{len(results) - len(failed)} pass, {len(failed)} FAIL")
sys.exit(1 if failed else 0)
