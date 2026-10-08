#!/usr/bin/env python3
"""Run -> Align switch soak on the PZ7035 through the Studio UI (#629): N flips, one line each.

  python3 scripts/yofo/e2e_run_align_flips.py http://127.0.0.1:8427 OUTDIR [--flips 10] [--hold-seconds 4]
                                              [--take-control]

Each flip opens the Experiment tab (Run, LED 7/60), holds, opens Camera & Alignment (Align) and
waits for a whole-frame preview. Reports per flip: ok/FAIL, seconds to the preview, the mode notice
if the switch failed, and the Align recovery counters from `fetch_instrument_status`
(`mode.align_lock.receiver_clears` / `failures`). Exits non-zero if any flip failed. It never
touches the pumps and starts no experiment. `--take-control` claims the controller role over
the page's own socket (for a server whose build lacks the Take control banner, or when another
tab holds it): use it only when the operator has agreed. Needs the Run window placed first
(the acceptance script or the Camera tab does it; the backend restores the saved one).
"""
import json
import re
import sys
import time
from pathlib import Path

from playwright.sync_api import sync_playwright

args = [a for a in sys.argv[1:] if not a.startswith("--")]
base, outdir = args[0], Path(args[1])
flips = int(sys.argv[sys.argv.index("--flips") + 1]) if "--flips" in sys.argv else 10
hold = float(sys.argv[sys.argv.index("--hold-seconds") + 1]) if "--hold-seconds" in sys.argv else 4.0
take = "--take-control" in sys.argv
outdir.mkdir(parents=True, exist_ok=True)

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


def tab(page, name):
    page.locator(".stage-tab-name", has_text=re.compile(f"^{re.escape(name)}$")).click()


def side(page, label):
    row = page.locator(".side-row", has=page.locator(".k", has_text=label)).first
    return row.locator(".v").inner_text() if row.count() else ""


def preview_mean(page):
    return page.evaluate("""() => {
      const c = [...document.querySelectorAll('canvas')].find(c => c.width === 816 && c.height === 624);
      if (!c) return 0;
      const d = c.getContext('2d').getImageData(0, 0, 816, 624).data;
      let sum = 0; for (let i = 0; i < d.length; i += 4) sum += d[i];
      return sum / (d.length / 4);
    }""")


seq = [800000]


def status(page):
    seq[0] += 1
    rid = seq[0]
    page.evaluate("(rid) => window.__ws.send(JSON.stringify({request_id: rid, cmd: 'fetch_instrument_status', args: null}))", rid)
    for _ in range(50):
        r = page.evaluate("(rid) => window.__replies[rid] || null", rid)
        if r:
            return (r.get("ok") or {}).get("mode", {})
        time.sleep(0.1)
    return {}


results = []
with sync_playwright() as p:
    browser = p.chromium.launch(args=["--no-sandbox"])
    page = browser.new_page(viewport={"width": 1400, "height": 1000})
    page.add_init_script(INIT)
    page.goto(base + "/")
    page.wait_for_function("() => /backend: ready/.test(document.body.innerText)", timeout=40000)
    time.sleep(2)
    for n in range(1, flips + 1):
        tab(page, "Experiment")
        run_ok = False
        for _ in range(25):
            time.sleep(1)
            if "run" in side(page, "LED:") and not page.locator(".mode-notice").count():
                run_ok = True
                break
        time.sleep(hold)
        before = (status(page).get("align_lock") or {}).get("receiver_clears", 0)
        t0 = time.time()
        tab(page, "Camera & Alignment")
        ok, note = False, ""
        for _ in range(60):
            time.sleep(0.5)
            if "align" in side(page, "LED:") and preview_mean(page) > 20:
                ok = True
                break
            notices = page.locator(".mode-notice").all_inner_texts()
            if notices:
                note = " / ".join(notices).replace("\n", " ")[:200]
                break
        took = time.time() - t0
        lock = status(page).get("align_lock") or {}
        row = {"flip": n, "run": run_ok, "align": ok, "seconds": round(took, 1),
               "resets": lock.get("receiver_clears", 0) - before, "failures": lock.get("failures"), "notice": note}
        results.append(row)
        print(("ok  " if ok else "FAIL"), json.dumps(row), flush=True)
        page.screenshot(path=str(outdir / f"flip{n:02d}.png"))
    browser.close()

(outdir / "flips.json").write_text(json.dumps(results, indent=2))
good = sum(1 for r in results if r["align"])
total_resets = sum(r["resets"] for r in results)
print(f"\n{good}/{len(results)} Run->Align switches locked; receiver resets issued: {total_resets}")
sys.exit(0 if good == len(results) else 1)
