#!/usr/bin/env python3
"""Browser acceptance of YOFO Studio on the PZ7035 (#501): one pass/fail per item.

  python3 scripts/yofo/e2e_studio_acceptance.py http://127.0.0.1:8427 OUTDIR [--run-seconds 10]
  python3 scripts/yofo/e2e_studio_acceptance.py http://127.0.0.1:8500 OUTDIR --desktop   # a host server

Needs Python Playwright with Chromium on the host and the board's server reached through an SSH
tunnel (the unit listens on 127.0.0.1 without a token). It switches camera modes and runs one short
experiment (the LED at the app's presets); it never touches the pumps: that item stays "pending
Gavin" until he is present. Writes OUTDIR/acceptance.json and screenshots, and exits non-zero if an
item failed.

Items, in order: token/Connect, Preflight with 0 warnings, Align, Run at 5 kHz, Run<->Align with no
shell commands, no MIB-only surfaces, pumps (pending). `--desktop` checks the host (MIB desktop)
capabilities instead: connect, and no PZ7035-only surfaces.
"""
import json
import re
import sys
import time
from pathlib import Path

from playwright.sync_api import sync_playwright

args = [a for a in sys.argv[1:] if not a.startswith("--")]
flags = [a for a in sys.argv[1:] if a.startswith("--")]
base, outdir = args[0], Path(args[1])
desktop = "--desktop" in flags
run_s = float(sys.argv[sys.argv.index("--run-seconds") + 1]) if "--run-seconds" in sys.argv else 10.0
outdir.mkdir(parents=True, exist_ok=True)
results = []

# What the PZ7035 must not show (the host-only surfaces from the 2026-10-07 E2E pass, #550).
MIB_ONLY = ["Nanopositioner", "Autofocus", "Auto background", "Clear Background", "Set Background", "Clear ROI",
            "Startup hardware selection", "EGrabber", "Calibrate Background", "Sort Trigger", "Periodic Test"]
# What only the PZ7035 shows (must be absent on the desktop).
PZ_ONLY = ["PL core", "Science=PL"]


def record(name, ok, detail=""):
    results.append({"item": name, "result": "pass" if ok else "FAIL", "detail": detail})
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}", flush=True)


def body(page):
    return page.inner_text("body")


def tab(page, name):
    page.locator(".stage-tab-name", has_text=re.compile(f"^{re.escape(name)}$")).click()


def side(page, label):
    """The sidebar value for a row label ('Sensor:' -> '816×624 @ 400.1 fps')."""
    row = page.locator(".side-row", has=page.locator(".k", has_text=label)).first
    return row.locator(".v").inner_text() if row.count() else ""


def canvas_stats(page):
    return page.evaluate("""() => {
      const c = [...document.querySelectorAll('canvas')].find(c => c.width === 816 && c.height === 624);
      if (!c) return null;
      const d = c.getContext('2d').getImageData(0, 0, 816, 624).data;
      let sum = 0; for (let i = 0; i < d.length; i += 4) sum += d[i];
      return {mean: sum / (d.length / 4)};
    }""")


with sync_playwright() as p:
    browser = p.chromium.launch(args=["--no-sandbox"])
    page = browser.new_page(viewport={"width": 1400, "height": 1000})
    page.on("dialog", lambda d: d.accept(str(outdir / "acceptance_run.h5")) if d.type == "prompt" else d.accept())
    errors = []
    page.on("pageerror", lambda e: errors.append(str(e)[:200]))

    # 1. Token / Connect: the UI connects without a prompt and the backend is ready.
    page.goto(base + "/")
    try:
        page.wait_for_function("() => /backend: ready/.test(document.body.innerText)", timeout=30000)
        prompt = "Access token" in body(page)
        record("token/Connect", not prompt, "connected without a token prompt; backend ready" if not prompt else "token prompt shown")
    except Exception as e:  # noqa: BLE001
        record("token/Connect", False, f"backend did not become ready: {e}")
    page.screenshot(path=str(outdir / "1_connect.png"))

    if desktop:
        text = body(page)
        leaked = [s for s in PZ_ONLY if s in text]
        record("MIB desktop unchanged (no PZ7035-only surfaces)", not leaked, f"leaked: {leaked}" if leaked else "none of " + ", ".join(PZ_ONLY))
    else:
        # 2. Preflight with 0 warnings.
        tab(page, "Hardware Preflight")
        time.sleep(4)
        text = body(page)
        m = re.search(r"(\d+) passed · (\d+) warning · (\d+) failed", text)
        page.screenshot(path=str(outdir / "2_preflight.png"))
        if m:
            passed, warn, failed = map(int, m.groups())
            detail = f"{passed} passed · {warn} warning · {failed} failed"
            record("Preflight with 0 warnings", warn == 0 and failed == 0, detail)
        else:
            record("Preflight with 0 warnings", False, "summary line not found")

        # 7 (checked here, on the first tabs): no MIB-only surfaces.
        text_all = text
        for name in ["Camera & Alignment", "Experiment", "Review"]:
            tab(page, name)
            time.sleep(1.5)
            text_all += "\n" + body(page)
        leaked = sorted({s for s in MIB_ONLY if s in text_all})
        record("no MIB-only surfaces", not leaked, f"found: {leaked}" if leaked else "none of " + ", ".join(MIB_ONLY))

        # 3. Align: whole frames, LED at the Align preset, sensor at 400 fps.
        tab(page, "Camera & Alignment")
        got = None
        for _ in range(40):
            time.sleep(1)
            s = canvas_stats(page)
            if s and s["mean"] > 20:
                got = s
                break
        led, sensor = side(page, "LED:"), side(page, "Sensor:")
        page.screenshot(path=str(outdir / "3_align.png"))
        ok = bool(got) and "align" in led and "400" in sensor
        record("Align", ok, f"frame mean {got['mean']:.0f} DN" if got else "no frame", ) if ok else \
            record("Align", False, f"frame={got} led='{led}' sensor='{sensor}' notices={[t for t in page.locator('.mode-notice').all_inner_texts()]}")

        # 4. Run at 5 kHz: place the window, switch, preview, a short experiment.
        window_ok = False
        try:
            page.get_by_label("Camera window x").fill("232")
            page.get_by_label("Camera window y").fill("336")
            page.get_by_role("button", name="Save camera ROI").click()
            window_ok = True
        except Exception:  # noqa: BLE001
            pass
        tab(page, "Experiment")
        run_line = ""
        for _ in range(30):
            time.sleep(1)
            m = re.search(r"Run 512×96 at \((\d+), (\d+)\)[^\n]*", body(page))
            if m:
                run_line = m.group(0)
                break
        sensor, led = side(page, "Sensor:"), side(page, "LED:")
        start = page.get_by_role("button", name="Start Experiment")
        started = False
        if run_line and start.is_enabled():
            start.click()
            started = True
            time.sleep(run_s)
            stop = page.get_by_role("button", name="Stop Experiment")
            if stop.is_enabled():
                stop.click()
            time.sleep(6)
        final = body(page)
        page.screenshot(path=str(outdir / "4_run.png"))
        fps = re.search(r"@ ([\d.]+) fps", sensor)
        rate_ok = bool(fps) and 4500 <= float(fps.group(1)) <= 5500
        failed_run = re.search(r"Run failed: [^\n]*", final)
        detail = f"{run_line} · sensor '{sensor}' · LED '{led}' · experiment {'started' if started else 'not started'}"
        if failed_run:
            detail += f" · {failed_run.group(0)}"
        record("Run at 5 kHz", window_ok and bool(run_line) and rate_ok and started and not failed_run, detail)

        # 5. Run <-> Align with no shell commands: the tabs switch the modes, twice.
        flips = []
        for name, want in [("Camera & Alignment", "align"), ("Experiment", "run"), ("Camera & Alignment", "align"), ("Experiment", "run")]:
            tab(page, name)
            ok_flip = False
            for _ in range(25):
                time.sleep(1)
                if want in side(page, "LED:") and not page.locator(".mode-notice").count():
                    ok_flip = True
                    break
            flips.append(f"{want}:{'ok' if ok_flip else 'NO'}")
        page.screenshot(path=str(outdir / "5_flips.png"))
        record("Run<->Align with no shell commands", all(f.endswith("ok") for f in flips), " ".join(flips))

        record("Pumps (rpm, start/stop, CW/CCW)", False, "pending Gavin: commanding the pumps needs him present")
        results[-1]["result"] = "pending"

    record("no page errors", not errors, "; ".join(errors[:3]))
    browser.close()

(outdir / "acceptance.json").write_text(json.dumps(results, indent=2))
failed = [r for r in results if r["result"] == "FAIL"]
print(f"\n{len(results) - len(failed)} ok/pending, {len(failed)} failed")
sys.exit(1 if failed else 0)
