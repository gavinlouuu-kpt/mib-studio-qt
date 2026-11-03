#!/usr/bin/env python3
"""Drive every MIB Studio guided tour end to end in headless Chromium, against the real `yofo-studio-server`.

No hardware: the server runs with the mock camera (a folder of frames and a fake GenApi node map), on a fresh empty data
directory per tour, and the browser is a fresh context (empty localStorage) per tour and colour scheme. The script acts as
the operator: it uses the same controls the tours point at (Refresh, Write, Start run, Stop & finalize run, Create recipe,
Select HDF File..., Analyze population, Save new workspace). The tours themselves never click and never send a command.

What it asserts, per scheme and tour (see `drive-log.txt` and `manifest.json` for the evidence):
  1. Every action step waits in its waiting state (Next disabled, same step) for at least 2 s without the operator, then the
     operator uses the real control, and the "Done" text appears only AFTER the backend's reply to the relevant command was
     received (page-side WebSocket timestamps: send, reply, and the instant the status text changed).
  2. The tour sends no command: outside the operator's own control use, every command on the WebSocket is a read or a poll
     (an allowlist built from desktop/src/bridge.ts). Commands are listed per step in the manifest.
  3. Idle gate: with the camera acquiring (and again with an experiment running) an action step is shown as blocked, its control
     cannot be reached, and nothing is sent.
  4. Missing target: with a step's `data-tour` attribute removed the popover says why and offers Skip and Restart; both are used.
  5. Feedback checkpoint: a verdict and a note are stored; the Tours list exports them, and the exported file holds them.
  6. Every step is screenshotted in light and dark; flowcharts and evidence panels are steps of the tours and are in the set.

Output (default docs/evidence/2026-11-03-fold-in-tours/): <scheme>/<tour-id>/NN-<slug>.png, <scheme>/extra/*.png,
feedback/*.json, manifest.json, contact-sheet.html, contact-sheet-<scheme>.png, drive-log.txt.

Re-run (the whole run inside one systemd scope, so Chromium and the servers are bounded):
  systemd-run --user --scope -p MemoryMax=16G python3 scripts/tours/drive_tours.py
  python3 scripts/tours/drive_tours.py --schemes light --tours fold-in-e01-discovery,fold-in-i12-f12-workspace   # a subset

Needs Python Playwright with Chromium (the miniconda one on P5); Pillow is optional (PNG quantization, contact sheets).
Exit status 0 only if every check passed.
"""
import argparse
import atexit
import collections
import html
import io
import json
import os
import re
import signal
import socket
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

from playwright.sync_api import Error as PlaywrightError, TimeoutError as PlaywrightTimeout, sync_playwright

try:
    from PIL import Image
except ImportError:  # the PNGs are then saved as Chromium wrote them
    Image = None

REPO = Path(__file__).resolve().parents[2]
HANDOVER = Path("/mnt/hdd/developer-data/mib-studio-qt/handover/fold-in-tours")
TOURS = ["fold-in-e01-discovery", "fold-in-e02-features", "fold-in-e04-e06-runs", "fold-in-i02-recipes",
         "fold-in-f04-population", "fold-in-i12-f12-workspace"]
FEEDBACK_TITLE = "Was this tour clear?"
HOLD_S = 2.2  # an action step must keep waiting at least this long without the operator
VIEWPORT = {"width": 1440, "height": 900}
EVIDENCE_DATE = "2026-11-03"

# What a tour (or the app's own polling) may send outside the operator's control use: reads and polls only. Built from the
# commands desktop/src/bridge.ts has and what the app sends while idle: every `fetch_*`, the `*_status` / `*_status_json`
# polls, the record lists, preferences, ABI and init probes. `poll_events_exact` is answered locally by the transport.
POLL = re.compile(r"^(abi_version|is_initialized|poll_events_exact|get_preferences|queue_overflow_total|fetch_[a-z0-9_]+|"
                  r"[a-z0-9_]+_status(_json)?|calibration_records_list|calibration_record_get)$")
# Reads a view sends by itself when it is shown (the Align stage reads the camera's node map when it mounts). They change nothing on the
# camera; a tour that shows the view therefore shows them. Everything that is neither a poll nor one of these fails the run.
VIEW_READ = re.compile(r"^(camera_feature_tree|camera_feature_read)$")


def is_read(cmd: str) -> bool:
    return bool(POLL.match(cmd) or VIEW_READ.match(cmd))


# What the app sends by itself on a page load (before any tour exists); listed so a reader sees what is excluded.
LOAD_TIME_ONLY = {"init", "processing_core_command", "sync_wall_clock", "restore_startup_configuration", "shell_log",
                  "startup_discovery_set_preference", "monitoring_set_active", "start_camera_discovery"}

# In-page instrumentation: every WebSocket request and reply with page-clock timestamps, the popover's step changes and the action
# status text changes (a MutationObserver), and marks the script places. One clock for all of them (epoch ms), so "reply before
# success" is a comparison of page-side timestamps, not of two processes' clocks.
INSTRUMENT_JS = r"""
(() => {
  if (window.__tlog) return;
  const log = window.__tlog = [];
  const now = () => performance.timeOrigin + performance.now();
  const short = (v, n) => { try { const s = typeof v === 'string' ? v : JSON.stringify(v); return s === undefined ? '' : (s.length > n ? s.slice(0, n) + '...' : s); } catch (e) { return '?'; } };
  window.__mark = (label) => log.push({k: 'mark', t: now(), label});
  const Orig = window.WebSocket;
  function Wrapped(url, protocols) {
    const ws = protocols === undefined ? new Orig(url) : new Orig(url, protocols);
    const send = ws.send.bind(ws);
    ws.send = (d) => {
      if (typeof d === 'string') { try { const j = JSON.parse(d); log.push({k: 'send', t: now(), cmd: j.cmd, id: j.request_id, args: short(j.args, 240)}); } catch (e) {} }
      return send(d);
    };
    ws.addEventListener('message', (m) => {
      const t = now();
      if (typeof m.data === 'string') {
        try {
          const j = JSON.parse(m.data);
          if (typeof j.request_id === 'number') log.push({k: 'recv', t, id: j.request_id, ok: 'ok' in j, err: 'error' in j ? short(j.error, 300) : null, body: 'ok' in j ? short(j.ok, 1800) : null});
        } catch (e) {}
      } else if (m.data instanceof ArrayBuffer && m.data.byteLength >= 8) {
        log.push({k: 'recv', t, id: Number(new DataView(m.data).getBigUint64(0, true)), ok: true, bin: true, bytes: m.data.byteLength});
      }
    });
    return ws;
  }
  Wrapped.prototype = Orig.prototype;
  Object.assign(Wrapped, {CONNECTING: 0, OPEN: 1, CLOSING: 2, CLOSED: 3});
  window.WebSocket = Wrapped;
  let lastStatus = null, lastPop = null;
  const observe = () => {
    const s = document.querySelector('.tour-action-status');
    const st = s ? s.textContent : null;
    if (st !== lastStatus) { lastStatus = st; log.push({k: 'status', t: now(), text: st}); }
    const p = document.querySelector('.driver-popover');
    const key = p ? `${p.querySelector('.driver-popover-progress-text')?.textContent ?? ''}|${p.querySelector('.driver-popover-title')?.textContent ?? ''}` : null;
    if (key !== lastPop) { lastPop = key; log.push({k: 'pop', t: now(), key}); }
  };
  const start = () => new MutationObserver(observe).observe(document.documentElement, {subtree: true, childList: true, characterData: true});
  if (document.documentElement) start(); else document.addEventListener('DOMContentLoaded', start);
})();
"""

POPOVER_JS = r"""
() => {
  const pop = document.querySelector('.driver-popover');
  if (!pop || getComputedStyle(pop).display === 'none') return null;
  const q = (s) => pop.querySelector(s);
  const act = document.querySelector('.driver-active-element');
  const region = document.querySelector('.tour-canvas-region');
  const status = q('.tour-action-status');
  const box = (el) => { if (!el) return null; const r = el.getBoundingClientRect(); return [Math.round(r.x), Math.round(r.y), Math.round(r.width), Math.round(r.height)]; };
  return {
    title: q('.driver-popover-title')?.textContent ?? '', progress: q('.driver-popover-progress-text')?.textContent ?? '',
    description: q('.driver-popover-description')?.textContent ?? '',
    status: status ? status.textContent : null,
    next_disabled: !!q('.driver-popover-next-btn')?.disabled, next_text: q('.driver-popover-next-btn')?.textContent ?? '',
    prev_disabled: !!q('.driver-popover-prev-btn')?.disabled,
    buttons: [...pop.querySelectorAll('.tour-btn')].map((b) => b.textContent),
    target: act ? (act.getAttribute('data-tour') ?? (act.id === 'driver-dummy-element' ? null : act.tagName.toLowerCase())) : (region ? 'region:' + region.dataset.tourRegion : null),
    target_no_interaction: act ? act.classList.contains('driver-no-interaction') : null,
    flow_open: !!document.querySelector('.tour-flow-modal'),
    pop_box: box(pop), act_box: box(act),
  };
}
"""


# ----------------------------------------------------------------------------------------------------------------------
# Small utilities
# ----------------------------------------------------------------------------------------------------------------------
T0 = time.time()
LOG_LINES: list[str] = []
LOG_FILE = None


def log(msg: str = "") -> None:
    line = f"[{time.time() - T0:7.1f}s] {msg}"
    print(line, flush=True)
    LOG_LINES.append(line)
    if LOG_FILE:
        LOG_FILE.write(line + "\n")
        LOG_FILE.flush()


def slug(text: str, limit: int = 44) -> str:
    return re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")[:limit].strip("-") or "step"


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def explain(e: Exception) -> str:
    """A Playwright failure in one line, with the element that intercepted the pointer when it says so (an overlay in the way)."""
    lines = str(e).split("\n")
    why = next((l.strip() for l in lines if "intercepts pointer events" in l or "from <" in l), "")
    m = re.search(r'class="(driver-[a-z-]+)', why) or re.search(r"<(\w+)[^>]*class=\"([^\"]*)", why)
    return (lines[0][:160] + (f" | blocked by the Driver.js overlay ({m.group(1)})" if "driver" in why else (f" | {why[:140]}" if why else ""))).strip()


def ms(t: float | None, base: float) -> float | None:
    return None if t is None else round(t - base, 1)


class Check:
    """Collects pass/fail checks of one scenario; a failed check never stops the drive (the evidence of the rest is still wanted)."""

    def __init__(self, scope: str):
        self.scope, self.items = scope, []

    def ok(self, name: str, cond: bool, detail: str = "") -> bool:
        self.items.append({"check": name, "result": "pass" if cond else "FAIL", "detail": detail})
        log(f"  [{'PASS' if cond else 'FAIL'}] {self.scope}: {name}" + (f" ({detail})" if detail else ""))
        return bool(cond)

    @property
    def failed(self) -> list[dict]:
        return [i for i in self.items if i["result"] == "FAIL"]


# ----------------------------------------------------------------------------------------------------------------------
# The server
# ----------------------------------------------------------------------------------------------------------------------
class Server:
    """One `yofo-studio-server` with the mock camera on a fresh data directory; always stopped with SIGTERM."""

    def __init__(self, cfg, name: str):
        self.cfg, self.name = cfg, name
        self.port = cfg.port or free_port()
        self.data = Path(cfg.run_data) / f"{time.strftime('%Y%m%dT%H%M%S')}-{name}"
        self.proc = None
        self.url = f"http://127.0.0.1:{self.port}"

    def start(self):
        c = self.cfg
        self.data.mkdir(parents=True, exist_ok=False)
        env = {  # keeps conda libraries out of the server (HDF5 comes from the system)
            "PATH": "/home/gavin/.local/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
            "HOME": os.environ.get("HOME", "/home/gavin"), "LANG": "C.UTF-8",
            "MIB_CAMERA_MODE": "mock", "MIB_MOCK_CAMERA_DIR": str(c.mock_dir), "MIB_MOCK_CAMERA_INTERVAL_MS": "10",
            "LD_LIBRARY_PATH": "/usr/lib/x86_64-linux-gnu/hdf5/serial",
            "MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL": "file:///nonexistent/mib-lut-manifest.json",
        }
        self.logfile = open(self.data / "server.log", "wb")
        self.proc = subprocess.Popen(
            [str(c.server), "--listen", f"127.0.0.1:{self.port}", "--dist", str(c.dist), "--no-token", "--data-dir", str(self.data / "data")],
            env=env, stdout=self.logfile, stderr=subprocess.STDOUT, start_new_session=True)
        (self.data / "data").mkdir(exist_ok=True)
        for _ in range(300):
            if self.proc.poll() is not None:
                raise RuntimeError(f"server exited early (code {self.proc.returncode}); see {self.data / 'server.log'}")
            try:
                if urllib.request.urlopen(self.url + "/", timeout=1).status == 200:
                    log(f"server {self.name}: {self.url}, data {self.data / 'data'} (pid {self.proc.pid})")
                    return self
            except OSError:
                time.sleep(0.1)
        raise RuntimeError("server did not answer")

    def stop(self):
        if not self.proc:
            return
        if self.proc.poll() is None:
            try:
                os.killpg(self.proc.pid, signal.SIGTERM)
                self.proc.wait(timeout=15)
            except (ProcessLookupError, subprocess.TimeoutExpired):
                try:
                    os.killpg(self.proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
        log(f"server {self.name}: stopped (exit {self.proc.returncode})")
        self.logfile.close()
        self.proc = None


class ExternalServer:
    """--url: a server somebody else runs. No restarts between tours, so state carries over; for debugging only."""

    def __init__(self, url: str):
        self.url, self.name, self.data = url.rstrip("/"), "external", Path("/nonexistent")

    def start(self):
        return self

    def stop(self):
        pass


LIVE_SERVERS: list = []


def _stop_all():
    for s in list(LIVE_SERVERS):
        s.stop()
        LIVE_SERVERS.remove(s)


atexit.register(_stop_all)
signal.signal(signal.SIGTERM, lambda *_: (_stop_all(), sys.exit(143)))


# ----------------------------------------------------------------------------------------------------------------------
# Screenshots
# ----------------------------------------------------------------------------------------------------------------------
class Shots:
    def __init__(self, out: Path, quantize: bool):
        self.out, self.quantize = out, quantize and Image is not None
        self.bytes = 0
        self.count = 0

    def save(self, png: bytes, rel: str) -> int:
        path = self.out / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        if self.quantize:
            img = Image.open(io.BytesIO(png)).convert("RGB").quantize(colors=256, method=Image.Quantize.FASTOCTREE, dither=Image.Dither.NONE)
            img.save(path, optimize=True)
        else:
            path.write_bytes(png)
        size = path.stat().st_size
        self.bytes += size
        self.count += 1
        return size


# ----------------------------------------------------------------------------------------------------------------------
# The page driver: one browser context per tour and scheme
# ----------------------------------------------------------------------------------------------------------------------
class Session:
    """A fresh context + page with the in-page instrumentation, the WebSocket log and helpers the tours' handlers use."""

    def __init__(self, cfg, browser, shots: Shots, scheme: str, scenario: str, server):
        self.cfg, self.shots, self.scheme, self.scenario, self.server = cfg, shots, scheme, scenario, server
        self.ctx = browser.new_context(viewport=VIEWPORT, color_scheme=scheme, reduced_motion="reduce", accept_downloads=True)
        self.ctx.add_init_script(INSTRUMENT_JS)
        self.page = self.ctx.new_page()
        self.events: list[dict] = []
        self.cmd_of: dict[int, str] = {}
        self.sent_of: dict[int, float] = {}
        self.pw_sent = 0
        self.open_requests: set[int] = set()
        self.dialogs: list[dict] = []
        self.prompt_answers: list[str] = []
        self.console: list[str] = []
        self.rows: list[dict] = []
        self.shots_taken: list[dict] = []
        self.checks = Check(scenario)
        self.page.on("dialog", self._dialog)
        self.page.on("pageerror", lambda e: self.console.append(f"pageerror: {str(e)[:300]}"))
        self.page.on("console", lambda m: self.console.append(f"console.{m.type}: {m.text[:300]}") if m.type == "error" else None)
        self.page.on("websocket", self._ws)

    def _ws(self, ws):
        ws.on("framesent", lambda d: setattr(self, "pw_sent", self.pw_sent + 1))

    def _dialog(self, d):
        row = {"type": d.type, "message": d.message[:240], "default": d.default_value[:160]}
        self.dialogs.append(row)
        if d.type == "prompt" and self.prompt_answers:
            ans = self.prompt_answers.pop(0)
            row["answered"] = ans
            log(f"    dialog prompt: {d.message[:90]!r} -> {ans}")
            d.accept(ans)
        elif d.type == "prompt":
            row["answered"] = None
            log(f"    dialog prompt with no answer queued: {d.message[:90]!r} (dismissed)")
            d.dismiss()
        else:
            row["answered"] = "accept"
            d.accept()

    # -- log ------------------------------------------------------------------------------------------------------
    def pull(self):
        try:
            new = self.page.evaluate("() => window.__tlog ? window.__tlog.splice(0) : []")
        except PlaywrightError:
            return
        for e in new:
            if e["k"] == "send":
                self.cmd_of[e["id"]], self.sent_of[e["id"]] = e["cmd"], e["t"]
                if not is_read(e["cmd"]):
                    self.open_requests.add(e["id"])
            elif e["k"] == "recv":
                e["cmd"] = self.cmd_of.get(e["id"])
                self.open_requests.discard(e["id"])
        self.events.extend(new)

    def mark(self, label: str):
        self.page.evaluate("l => window.__mark(l)", label)

    def now_ms(self) -> float:
        return self.page.evaluate("() => performance.timeOrigin + performance.now()")

    # -- facts ----------------------------------------------------------------------------------------------------
    def popover(self):
        return self.page.evaluate(POPOVER_JS)

    def settle(self, timeout_s: float = 15.0):
        """The popover is on screen and neither it nor the highlighted element has moved for ~250 ms."""
        end, last, stable = time.time() + timeout_s, None, 0
        while time.time() < end:
            f = self.popover()
            sig = (f["pop_box"], f["act_box"], f["title"], f["status"]) if f else None
            if sig is not None and sig == last:
                stable += 1
                if stable >= 3:
                    return f
            else:
                stable, last = 0, sig
            self.page.wait_for_timeout(80)
        raise PlaywrightTimeout("the popover did not settle")

    def wait_step_change(self, prev_key: str, timeout_s: float = 20.0):
        end = time.time() + timeout_s
        while time.time() < end:
            f = self.popover()
            if f and f"{f['progress']}|{f['title']}" != prev_key:
                return self.settle()
            self.page.wait_for_timeout(60)
        raise PlaywrightTimeout(f"the step did not change from {prev_key!r}")

    # -- screenshots ----------------------------------------------------------------------------------------------
    def shot(self, rel_dir: str, name: str, **row):
        rel = f"{rel_dir}/{name}.png"
        size = self.shots.save(self.page.screenshot(), rel)
        row.update(path=rel, bytes=size)
        self.shots_taken.append(row)
        return rel

    # -- operator helpers: these do what a person does (click, type), with Playwright's actionability and hit-target checks ---
    def click(self, sel_or_loc, timeout=6000):
        loc = self.page.locator(sel_or_loc).first if isinstance(sel_or_loc, str) else sel_or_loc
        loc.click(timeout=timeout)

    def type_into(self, loc, text: str, timeout=6000):
        """Click the field (so an overlay in the way shows up as a failure), select its text and type."""
        loc.click(timeout=timeout)
        self.page.keyboard.press("Control+A")
        self.page.keyboard.type(text, delay=8)

    def start_camera(self):
        """The operator starts the mock camera in the classic view, then returns to the workspace shell."""
        self.page.get_by_role("button", name="Classic view").click()
        self.page.get_by_role("button", name="Start Camera").click(timeout=8000)
        self.page.wait_for_function("() => /Camera=running/.test(document.body.innerText)", timeout=20000)
        self.page.wait_for_timeout(600)
        self.page.get_by_role("button", name="Workspace preview").click()
        self.page.wait_for_timeout(500)

    def stop_camera(self):
        self.page.get_by_role("button", name="Classic view").click()
        self.page.get_by_role("button", name="Stop Camera").click(timeout=8000)
        self.page.wait_for_function("() => !/Camera=running/.test(document.body.innerText)", timeout=20000)
        self.page.get_by_role("button", name="Workspace preview").click()
        self.page.wait_for_timeout(300)

    def load_app(self):
        self.page.goto(self.server.url + "/")
        self.page.wait_for_function("() => /backend: ready/.test(document.body.innerText)", timeout=40000)
        self.page.get_by_role("button", name="Tours").wait_for(timeout=15000)
        self.page.wait_for_timeout(1500)  # the notice and the lazy tours chunk

    def open_list(self):
        self.page.get_by_role("button", name="Tours", exact=True).click()
        self.page.locator('[role=dialog][aria-label="Tours and what\'s new"]').wait_for(timeout=10000)
        self.page.wait_for_timeout(300)

    def close(self):
        try:
            self.ctx.close()
        except PlaywrightError:
            pass


# ----------------------------------------------------------------------------------------------------------------------
# Seeding through the WebSocket (setup only: what the I02 recipe form needs to exist on the instrument)
# ----------------------------------------------------------------------------------------------------------------------
SEED_JS = r"""
async ({url, calls}) => {
  const ws = new WebSocket(url);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = () => rej('open failed'); });
  const pending = new Map(); let next = 1; let session = null;
  ws.onmessage = (m) => { if (typeof m.data !== 'string') return; const j = JSON.parse(m.data);
    if (j.session) session = j.session;
    if (typeof j.request_id === 'number' && pending.has(j.request_id)) { pending.get(j.request_id)(j); pending.delete(j.request_id); } };
  const rpc = (cmd, args) => new Promise((res) => { const id = next++; pending.set(id, res); ws.send(JSON.stringify({request_id: id, cmd, args: args ?? {}})); });
  const out = [];
  let prev = null;
  for (const c of calls) {
    const args = typeof c.args === 'function' ? c.args(prev) : c.args;
    prev = await rpc(c.cmd, args); out.push({cmd: c.cmd, reply: prev});
    if (c.stop_on_error && 'error' in prev) break;
  }
  ws.close();
  return out;
}
"""


# ----------------------------------------------------------------------------------------------------------------------
# The tour player: opens the list, plays a tour, acts as the operator where the tour asks, asserts, screenshots
# ----------------------------------------------------------------------------------------------------------------------
class Operator:
    """What the operator does at a step, keyed by the highlighted control's data-tour value. `action` handlers do what an action
    step asks and name the commands that must answer it; `assist` handlers press a control a non-action step describes."""

    def __init__(self, S: Session, tour_id: str, fixtures: Path, server):
        self.S, self.tour_id, self.fixtures, self.server = S, tour_id, fixtures, server
        self.seeded: dict = {}
        self.observations: dict = {}

    # ---- observations (what the screen said), recorded for the evidence report ----------------------------------------
    def observe(self, key: str, js: str):
        try:
            v = self.S.page.evaluate(js)
        except PlaywrightError as e:
            v = f"<could not read: {str(e)[:80]}>"
        self.observations[key] = v
        log(f"    observed {key}: {str(v)[:260]!r}")
        return v

    # ---- action steps ----------------------------------------------------------------------------------------------
    def action(self, target: str):
        """(operator action text, expected relevant commands, performer) for the action step whose control is `target`."""
        S, p = self.S, self.S.page
        t = self.tour_id

        def e01():
            S.click('[data-tour="connect-refresh"]')

        def e02():
            row = p.locator('[aria-label="Frame rate requested"]')
            S.type_into(row, self.seeded.get("fps", "334"))
            S.click('[aria-label="Write Frame rate"]')

        def e04_start():
            S.prompt_answers.append(str(self.server.data / "data" / "runs" / "tour-run.h5"))
            S.click('[data-tour="plan-start-run"]')

        def e04_frame():
            pass  # nothing to press: the tour waits for the run's status to show a frame counter

        def e04_stop():
            S.click('[data-tour="live-stop-run"]')

        def i02():
            s = self.seeded
            for key, val in (("title", "Tour drive recipe"), ("methodId", s["method_id"]), ("methodRevision", s["method_revision"]),
                             ("planId", s["plan_id"]), ("planRevision", s["plan_revision"])):
                S.type_into(p.locator(f'[aria-label="New recipe {key}"]'), val)
            S.click('[data-tour="recipe-create"]')

        def f04_open():
            S.prompt_answers.append(str(self.fixtures / "population-6000.h5"))
            S.click('[data-tour="review-open-file"]')

        def f04_analyze():
            S.click('[data-tour="review-population-analyze"]')

        def i12():
            S.click('[data-tour="saved-plan-save"]')

        table = {
            ("fold-in-e01-discovery", "connect-refresh"): ("press Refresh on the Connect tab", ["start_camera_discovery", "start_device_discovery", "fetch_device_discovery"], e01),
            ("fold-in-e02-features", "align-quick-controls"): ("type a frame rate and press Write on the Frame rate row", ["camera_feature_write"], e02),
            ("fold-in-e04-e06-runs", "plan-start-run"): ("press Start run and answer the file prompt", ["experiment_start", "start_experiment"], e04_start),
            ("fold-in-e04-e06-runs", "live-body"): ("nothing to press: watch the Live view", [], e04_frame),
            ("fold-in-e04-e06-runs", "live-stop-run"): ("press Stop & finalize run", ["experiment_stop", "fetch_run_accounting"], e04_stop),
            ("fold-in-i02-recipes", "recipe-form"): ("fill the New recipe form and press Create recipe", ["recipe_create"], i02),
            ("fold-in-f04-population", "review-open-file"): ("press Select HDF File... and answer the path prompt", ["load_recording"], f04_open),
            ("fold-in-f04-population", "review-population-analyze"): ("press Analyze population", ["analyze_population"], f04_analyze),
            ("fold-in-i12-f12-workspace", "saved-plan-save"): ("press Save new workspace", ["workspace_write"], i12),
        }
        return table.get((t, target))

    # ---- assists (a control a non-action step describes) --------------------------------------------------------------
    def assist(self, target: str):
        S, p = self.S, self.S.page
        t = self.tour_id
        if (t, target) == ("fold-in-e02-features", "align-feature-toggle"):
            return ("press Feature tree", ["camera_feature_tree"], lambda: S.click('[data-tour="align-feature-toggle"]'))
        if (t, target) == ("fold-in-i02-recipes", "recipe-profile"):
            return ("type the local profile directory", [], lambda: S.type_into(p.locator('[aria-label="Recipe profile directory"]'), self.seeded["profile_base"]))
        if (t, target) == ("fold-in-i02-recipes", "recipe-compare"):
            return ("press Compare with the instrument", ["recipe_diff"], lambda: S.click('[data-tour="recipe-compare"]'))
        if (t, target) == ("fold-in-i02-recipes", "recipe-preview-apply"):
            return ("press Preview apply", ["recipe_apply_preview"], lambda: S.click('[data-tour="recipe-preview-apply"]'))
        if (t, target) == ("fold-in-f04-population", "review-population-gallery"):
            return ("press Load gated gallery", [],
                    lambda: p.get_by_role("button", name="Load gated gallery").click(timeout=6000))
        if (t, target) == ("fold-in-i12-f12-workspace", "saved-plan-history"):
            return ("press Read revision history", ["workspace_read"], lambda: S.click('[data-tour="saved-plan-history"]'))
        return None


class TourPlayer:
    def __init__(self, cfg, S: Session, tour_id: str, server, shot_dir: str):
        self.cfg, self.S, self.tour_id, self.server, self.dir = cfg, S, tour_id, server, shot_dir
        self.op = Operator(S, tour_id, Path(cfg.fixtures), server)
        self.steps: list[dict] = []
        self.actions: list[dict] = []
        self.n_steps = None
        self.step_dwell: dict[str, tuple[float, float]] = {}

    # ---- a short wait that lets the replies of a just-pressed control arrive, then closes the operator window --------------
    def end_window(self, label: str, settle_ms: int = 450, timeout_s: float = 10.0):
        """Close an operator window once every non-poll request sent in it has been answered (and a short settle after)."""
        S = self.S
        end = time.time() + timeout_s
        while time.time() < end:
            S.pull()
            if not S.open_requests:
                break
            S.page.wait_for_timeout(100)
        S.page.wait_for_timeout(settle_ms)
        S.mark(f"op-end:{label}")

    def command_times(self, since_t: float, cmds):
        """For commands in `cmds` sent after `since_t`: (command, sent_t, recv_t, reply ok, body) in order."""
        S = self.S
        S.pull()
        out = []
        recv_by_id = {e["id"]: e for e in S.events if e["k"] == "recv" and not e.get("bin")}
        for e in S.events:
            if e["k"] == "send" and e["t"] >= since_t and e["cmd"] in cmds:
                r = recv_by_id.get(e["id"])
                out.append({"cmd": e["cmd"], "id": e["id"], "sent_t": e["t"], "recv_t": r["t"] if r else None,
                            "ok": r["ok"] if r else None, "body": (r.get("body") or r.get("err")) if r else None, "args": e.get("args")})
        return out

    def first_frames_reply(self, before_t: float):
        """The first fetch_experiment_status reply (before `before_t`) whose counters show a frame: the page's own first-frame observation."""
        self.S.pull()
        for e in self.S.events:
            if e["k"] == "recv" and e.get("cmd") == "fetch_experiment_status" and e["t"] <= before_t and e.get("body"):
                hit = re.findall(r'"(valid_buffered|invalid_buffered|valid_saved|invalid_saved|persistence_admitted)"\s*:\s*"?([0-9]+)', e["body"])
                nz = [(k, v) for k, v in hit if int(v) > 0]
                if nz:
                    return {"reply_t_rel_ms": round(e["t"] - before_t, 1), "counters": dict(nz)}
        return None

    def status_events(self, since_t: float):
        self.S.pull()
        return [e for e in self.S.events if e["k"] == "status" and e["t"] >= since_t]

    # ---- one action step -----------------------------------------------------------------------------------------------
    def run_action(self, idx: int, f: dict, action, ck: Check):
        S, name = self.S, f"{idx + 1:02d}-{slug(f['title'])}"
        text, expect, perform = action
        row = {"step": idx + 1, "title": f["title"], "target": f["target"], "operator_action": text, "expected_commands": expect}
        waiting_status = f["status"] or ""
        ck.ok(f"step {idx + 1} '{f['title']}' waits (Next disabled, waiting text)",
              f["next_disabled"] and "Waiting for the backend to confirm" in waiting_status, f"status={waiting_status!r}")
        S.shot(self.dir, name, step=idx + 1, title=f["title"], kind="action-waiting")
        # ---- the hold: nothing happens for >= 2 s without the operator
        S.pull()
        t_hold0 = S.now_ms()
        key0 = f"{f['progress']}|{f['title']}"
        held = True
        end = time.time() + HOLD_S
        while time.time() < end:
            g = S.popover()
            if not g or f"{g['progress']}|{g['title']}" != key0 or not g["next_disabled"] or (g["status"] or "") != waiting_status:
                held = False
                break
            S.page.wait_for_timeout(100)
        S.pull()
        t_hold1 = S.now_ms()
        # the popover/status events inside the hold window must be only the ones present at its start
        moved = [e for e in S.events if e["k"] in ("pop", "status") and t_hold0 + 50 < e["t"] < t_hold1 and (
            (e["k"] == "pop" and e["key"] != key0) or (e["k"] == "status" and e["text"] != waiting_status))]
        held = held and not moved
        row["hold_ms"] = round(t_hold1 - t_hold0)
        row["hold_ok"] = held
        ck.ok(f"step {idx + 1} waiting held >= {HOLD_S:.1f} s with no operator input", held and (t_hold1 - t_hold0) >= HOLD_S * 1000 - 150,
              f"held {row['hold_ms']} ms, events in window: {moved[:2]}")
        # ---- the operator acts
        label = f"{self.tour_id}:{idx + 1}"
        S.mark(f"op-begin:{label}")
        S.pull()
        t_begin = S.now_ms()
        perform_error = None
        try:
            perform()
        except (PlaywrightError, PlaywrightTimeout) as e:
            perform_error = explain(e)
        row["operator_error"] = perform_error
        ck.ok(f"step {idx + 1} operator could use the real control ({text})", perform_error is None, perform_error or "")
        # ---- wait for the tour to say Done
        timeout = 90 if self.tour_id.endswith("e04-e06-runs") else 60
        done = None
        end = time.time() + timeout
        while time.time() < end and perform_error is None:
            g = S.popover()
            if g and g["status"] and g["status"].startswith("Done:"):
                done = g
                break
            if g and (g["title"].startswith("What happened") or g["title"].startswith("Not on screen")):
                done = g
                break
            S.page.wait_for_timeout(50)
        S.pull()
        if done is None:
            g = S.popover()
            row["result"] = "no-success"
            row["final_popover"] = {"title": g["title"], "status": g["status"], "description": g["description"][:400]} if g else None
            ck.ok(f"step {idx + 1} success shown after the operator's action", False,
                  f"popover now: {(g or {}).get('title')!r} / {(g or {}).get('status')!r}")
            S.shot(self.dir, name + "-no-success", step=idx + 1, title=f["title"], kind="action-failed")
            self.end_window(label)
            self.actions.append(row)
            return False
        if done["title"].startswith(("What happened", "Not on screen")):
            row["result"] = "recovery"
            row["final_popover"] = {"title": done["title"], "description": done["description"][:500]}
            ck.ok(f"step {idx + 1} success shown after the operator's action", False, f"recovery popover: {done['description'][:200]!r}")
            S.shot(self.dir, name + "-recovery", step=idx + 1, title=f["title"], kind="action-recovery")
            self.end_window(label)
            self.actions.append(row)
            return False
        # timestamps (page clock): command sent/reply, status changed to Done
        done_events = [e for e in S.events if e["k"] == "status" and e["t"] >= t_begin and (e["text"] or "").startswith("Done:")]
        t_success = done_events[0]["t"] if done_events else None
        cmds = self.command_times(t_begin, set(expect)) if expect else []
        row["done_text"] = done["status"]
        row["commands"] = [{"cmd": c["cmd"], "sent_ms": ms(c["sent_t"], t_begin), "reply_ms": ms(c["recv_t"], t_begin), "reply_ok": c["ok"],
                            "args": (c["args"] or "")[:160], "reply_body": (c["body"] or "")[:300]} for c in cmds if c["cmd"] != "fetch_device_discovery"]
        polls = [c for c in cmds if c["cmd"] == "fetch_device_discovery"]
        if polls:
            row["commands"].append({"cmd": "fetch_device_discovery", "polls": len(polls), "last_reply_ms": ms(polls[-1]["recv_t"], t_begin),
                                    "last_reply_body": (polls[-1]["body"] or "")[:300]})
        row["success_shown_ms"] = ms(t_success, t_begin)
        # the relevant reply: for a discovery, the last poll reply before success; otherwise the last reply of an expected command
        rel = [c for c in cmds if c["recv_t"] is not None and t_success is not None and c["recv_t"] <= t_success]
        late = [c for c in cmds if c["recv_t"] is not None and t_success is not None and c["recv_t"] > t_success and c["cmd"] not in
                ("fetch_run_accounting", "fetch_device_discovery", "fetch_experiment_status")]
        row["relevant_reply_ms"] = ms(rel[-1]["recv_t"], t_begin) if rel else None
        row["reply_before_success"] = bool(rel) and not any(c["cmd"] in ("camera_feature_write", "recipe_create", "workspace_write", "load_recording",
                                                                            "analyze_population", "experiment_start", "start_experiment") for c in late)
        if not expect:  # nothing to press: the relevant evidence is a status reply with the counter, checked by the caller
            row["reply_before_success"] = None
        if expect:
            ck.ok(f"step {idx + 1} 'Done' appeared only after the backend's reply to {'/'.join(sorted({c['cmd'] for c in cmds}) or expect)}",
                  row["reply_before_success"], f"reply at +{row['relevant_reply_ms']} ms, success shown at +{row['success_shown_ms']} ms")
        if self.tour_id.endswith("e01-discovery"):
            m = re.search(r"instance ([0-9a-f-]{36})", done["status"] or "")
            bodies = " ".join(c["body"] or "" for c in cmds)
            row["instance_shown"] = m.group(1) if m else None
            ck.ok("the instance ID the tour shows is the one in the backend's reply", bool(m) and m.group(1) in bodies, f"shown {row['instance_shown']}")
        S.shot(self.dir, name + "-done", step=idx + 1, title=f["title"], kind="action-done")
        # the tour does not advance by itself after the success either
        S.page.wait_for_timeout(1000)
        g = S.popover()
        row["stays_after_done"] = bool(g and f"{g['progress']}|{g['title']}" == key0)
        ck.ok(f"step {idx + 1} stays on the step after Done until Next is pressed", row["stays_after_done"])
        self.end_window(label)
        S.pull()
        sent = [e["cmd"] for e in S.events if e["k"] == "send" and e["t"] >= t_begin and not is_read(e["cmd"])]
        row["operator_non_poll_commands"] = sorted(set(sent))
        log(f"    operator window non-poll commands: {sorted(set(sent))}")
        self.actions.append(row)
        row["result"] = "done"
        return True

    # ---- a non-action step with an operator assist ---------------------------------------------------------------------
    def run_assist(self, idx: int, f: dict, assist, ck: Check):
        S = self.S
        text, expect, perform = assist
        label = f"{self.tour_id}:{idx + 1}:assist"
        S.mark(f"op-begin:{label}")
        S.pull()
        t0 = S.now_ms()
        err = None
        try:
            perform()
        except (PlaywrightError, PlaywrightTimeout) as e:
            err = explain(e)
        ck.ok(f"step {idx + 1} operator assist: {text}", err is None, err or "")
        self.end_window(label, settle_ms=900)
        S.pull()
        sent = [e["cmd"] for e in S.events if e["k"] == "send" and e["t"] >= t0 and not is_read(e["cmd"])]
        self.steps[-1]["assist"] = {"action": text, "non_poll_commands": sorted(set(sent)), "error": err}
        if expect:
            odd = sorted(set(sent) - set(expect) - {"shell_log"})
            ck.ok(f"step {idx + 1} the operator's press sent only {sorted(expect)} (no write, no apply)", not odd, f"sent {sorted(set(sent))}")
        return err is None

    # ---- the walk ----------------------------------------------------------------------------------------------------
    def play(self, ck: Check, from_list=True, prep=None):
        S, tid = self.S, self.tour_id
        S.mark("tour-begin")
        entry = S.page.locator(f'li[data-tour-entry="{tid}"]')
        entry.get_by_role("button", name="Take the tour").click()
        f = S.wait_step_change("", 20)
        guard = 0
        while guard < 60:
            guard += 1
            S.pull()
            title = f["title"]
            m = re.match(r"(\d+) of (\d+)$", f["progress"] or "")
            if m and self.n_steps is None:
                self.n_steps = int(m.group(2))
            if title == FEEDBACK_TITLE:
                kind, idx = "feedback", self.n_steps or 0
            elif title.startswith("Not on screen"):
                kind, idx = "missing", int(m.group(1)) - 1 if m else -1
            elif title.startswith("What happened"):
                kind, idx = "recovery", int(m.group(1)) - 1 if m else -1
            else:
                kind, idx = "step", int(m.group(1)) - 1 if m else -1
            row = {"index": idx + 1, "of": self.n_steps, "kind": kind, "title": title, "target": f["target"], "flow_open": f["flow_open"],
                   "action": f["status"] is not None, "next_disabled": f["next_disabled"], "buttons": f["buttons"], "status": f["status"],
                   "pop_key": f"{f['progress']}|{title}", "shown_t": S.now_ms()}
            self.steps.append(row)
            log(f"  {row['index']:>2}/{self.n_steps} [{kind}] {title}  -> {f['target']}" + (f"  (status: {f['status'][:90]})" if f["status"] else ""))
            name = f"{(idx + 1):02d}-{slug(title)}"
            if kind == "feedback":
                return self.feedback(ck, f)
            if kind in ("missing", "recovery"):
                ck.ok(f"step {idx + 1} '{title}' is on screen (no missing target / recovery)", False, f["description"][:220])
                S.shot(self.dir, name, step=idx + 1, title=title, kind=kind)
            elif f["status"] is None:
                S.shot(self.dir, name, step=idx + 1, title=title, kind="step", target=f["target"])
                self.observe_step(idx, f)
                asst = self.op.assist(f["target"] or "")
                if asst:
                    ok = self.run_assist(idx, f, asst, ck)
                    if ok:
                        S.page.wait_for_timeout(250)
                        g = S.settle()
                        S.shot(self.dir, name + "-after", step=idx + 1, title=title, kind="after-assist")
                        self.observe_step(idx, {**g, "target": f["target"]}, after=True)
            else:
                S.observe_note = f["status"]
                act = self.op.action(f["target"] or "")
                if f["status"].startswith("Not now:"):
                    ck.ok(f"step {idx + 1} '{title}' can be done (not blocked)", False, f["status"])
                    S.shot(self.dir, name + "-blocked", step=idx + 1, title=title, kind="action-blocked")
                    self.actions.append({"step": idx + 1, "title": title, "result": "blocked", "status": f["status"]})
                elif f["status"].startswith("Already done:"):
                    # nothing to wait for: the fact was already confirmed (E04's first frame, once the run is accepted)
                    S.shot(self.dir, name, step=idx + 1, title=title, kind="action-already-done")
                    arow = {"step": idx + 1, "title": title, "result": "already-done", "status": f["status"], "target": f["target"]}
                    if f["target"] == "live-body":
                        first = self.first_frames_reply(row["shown_t"])
                        arow["first_status_reply_with_frames"] = first
                        ck.ok(f"step {idx + 1} 'Already done' (first frame) rests on a status reply with a nonzero frame counter that arrived earlier",
                              bool(first), json.dumps(first)[:300] if first else "no such reply before the step was shown")
                    self.actions.append(arow)
                elif act is None:
                    ck.ok(f"step {idx + 1} '{title}' has an operator routine in the drive", False, f"no routine for {f['target']}")
                    S.shot(self.dir, name, step=idx + 1, title=title, kind="action-unhandled")
                else:
                    self.run_action(idx, f, act, ck)
                    g = S.popover()
                    if g:
                        self.observe_step(idx, {**g, "target": f["target"]}, after=True)
            # ---- go on with Next (the tour's own control)
            S.pull()
            g = S.popover()
            if g is None:
                ck.ok("the popover is still there", False, "the tour ended unexpectedly")
                return False
            key = f"{g['progress']}|{g['title']}"
            if g["next_disabled"]:
                ck.ok(f"step {idx + 1} Next enabled to go on", False, "Next stays disabled; using Skip")
                S.page.locator(".driver-popover .tour-skip").click()
            else:
                S.page.locator(".driver-popover-next-btn").click()
            f = S.wait_step_change(key, 25)
        ck.ok("the tour reached its feedback checkpoint", False, "too many steps")
        return False

    # ---- what the screen said at a step, for the evidence report ------------------------------------------------------
    def observe_step(self, idx: int, f: dict, after: bool = False):
        o, t, tgt = self.op, self.tour_id, f["target"]
        js_text = "(sel) => { const e = document.querySelector(sel); return e ? e.innerText.replace(/\\s+/g,' ').trim().slice(0, 700) : null; }"
        def text(sel):
            return self.S.page.evaluate(js_text, sel)
        if t == "fold-in-e01-discovery":
            if tgt == "connect-devices": o.observations["devices list"] = text('[data-tour="connect-devices"]'); log(f"    observed devices list: {o.observations['devices list']!r}")
            if tgt == "connect-found":
                o.observations["found line"] = text('[data-tour="connect-found"]'); log(f"    observed found line: {o.observations['found line']!r}")
                o.observations["devices list after refresh"] = text('[data-tour="connect-devices"]'); log(f"    observed devices list after refresh: {o.observations['devices list after refresh']!r}")
        elif t == "fold-in-e02-features":
            if tgt == "align-feature-tree" and not after: o.observations["feature tree"] = text('[data-tour="align-feature-tree"]'); log(f"    observed feature tree: {o.observations['feature tree'][:300]!r}")
            if tgt == "align-write-reply": o.observations["write reply"] = text('[data-tour="align-write-reply"]'); log(f"    observed write reply: {o.observations['write reply']!r}")
            if tgt == "align-state-chip": o.observations["state chips"] = self.S.page.evaluate("() => [...document.querySelectorAll('[data-tour=\"align-state-chip\"]')].map(e => e.innerText + ' | ' + (e.title||''))"); log(f"    observed state chips: {o.observations['state chips']}")
        elif t == "fold-in-e04-e06-runs":
            if tgt == "run-summary-lifecycle": o.observations["lifecycle"] = text('[data-tour="run-summary-lifecycle"]'); log(f"    observed lifecycle: {o.observations['lifecycle']!r}")
            if tgt == "run-summary-counts": o.observations["counts"] = text('[data-tour="run-summary-counts"]'); log(f"    observed counts: {o.observations['counts']!r}")
            if tgt == "run-summary-writer": o.observations["writer"] = text('[data-tour="run-summary-writer"]'); log(f"    observed writer: {o.observations['writer']!r}")
        elif t == "fold-in-i02-recipes":
            if tgt == "recipe-diff": o.observations["diff"] = text('[data-tour="recipe-diff"]'); log(f"    observed diff: {o.observations['diff']!r}")
            if tgt == "recipe-apply-preview": o.observations["apply preview"] = text('[data-tour="recipe-apply-preview"]'); log(f"    observed apply preview: {o.observations['apply preview'][:400]!r}")
            if tgt == "recipe-form" and after: o.observations["recipe notice"] = self.S.page.evaluate("() => { const n = [...document.querySelectorAll('[data-testid=recipe-library] *')].filter(e => /Created recipe/.test(e.textContent) && e.children.length === 0).map(e => e.textContent); return n.slice(0,2); }"); log(f"    observed recipe notice: {o.observations['recipe notice']}")
        elif t == "fold-in-f04-population":
            if tgt == "review-population-result": o.observations["counts"] = text('[data-tour="review-population-result"]'); log(f"    observed counts: {o.observations['counts']!r}")
            if tgt == "review-population-statistics": o.observations["statistics"] = text('[data-tour="review-population-statistics"]'); log(f"    observed statistics: {o.observations['statistics']!r}")
            if tgt == "review-population-gallery":
                n = self.S.page.evaluate("() => { const g = document.querySelector('[data-tour=\"review-population-gallery\"]'); return g ? {imgs: g.querySelectorAll('img, canvas').length, text: g.innerText.replace(/\\s+/g,' ').slice(0, 300)} : null; }")
                o.observations["gallery" + (" after load" if after else "")] = n; log(f"    observed gallery{' after load' if after else ''}: {n}")
            if tgt == "review-open-file" and after: o.observations["review header"] = self.S.page.evaluate("() => document.body.innerText.match(/[^\\n]*valid[^\\n]*invalid[^\\n]*/i)?.[0] ?? null"); log(f"    observed review header: {o.observations['review header']!r}")
        elif t == "fold-in-i12-f12-workspace":
            if tgt == "saved-plan-message": o.observations["message"] = text('[data-tour="saved-plan-message"]'); log(f"    observed message: {o.observations['message']!r}")
            if tgt == "saved-plan-history" and after: o.observations["history"] = self.S.page.evaluate("() => { const s = document.querySelector('[data-testid=saved-plan]'); return s ? s.innerText.replace(/\\s+/g,' ').slice(-700) : null; }"); log(f"    observed history: {o.observations['history'][-300:]!r}")

    # ---- the feedback checkpoint ------------------------------------------------------------------------------------
    def feedback(self, ck: Check, f: dict):
        S, tid = self.S, self.tour_id
        verdict = "clear" if (TOURS.index(tid) % 2 == 0) else "unclear"
        note = f"drive {EVIDENCE_DATE} {S.scheme}: {verdict}; scripted note for {tid}"
        S.shot(self.dir, f"{(self.n_steps or 0) + 1:02d}-was-this-tour-clear", step=(self.n_steps or 0) + 1, title=f["title"], kind="feedback")
        ck.ok("feedback checkpoint shows Clear and Unclear and a note field",
              S.page.locator(".driver-popover .tour-verdict-clear").count() == 1 and S.page.locator(".driver-popover .tour-verdict-unclear").count() == 1
              and S.page.locator(".driver-popover .tour-feedback-note").count() == 1)
        S.page.locator(f".driver-popover .tour-verdict-{verdict}").click()
        S.page.locator(".driver-popover .tour-feedback-note").click()
        S.page.keyboard.type(note, delay=5)
        pressed = S.page.locator(f".driver-popover .tour-verdict-{verdict}").get_attribute("aria-pressed")
        ck.ok(f"verdict '{verdict}' is selected", pressed == "true")
        S.shot(self.dir, f"{(self.n_steps or 0) + 1:02d}-was-this-tour-clear-answered", step=(self.n_steps or 0) + 1, title=f["title"], kind="feedback-answered")
        S.page.locator(".driver-popover-next-btn").click()
        S.page.wait_for_function("() => !document.querySelector('.driver-popover')", timeout=10000)
        S.page.wait_for_timeout(500)
        S.mark("tour-end")
        S.pull()
        self.feedback_given = {"verdict": verdict, "note": note}
        return True


# ----------------------------------------------------------------------------------------------------------------------
# Command analysis (assertion 2)
# ----------------------------------------------------------------------------------------------------------------------
# What the app itself sends a moment after the operator's own action changed its state (not the tour: its code has no bridge call). Each
# entry names the operator window (tour:step) and the commands accepted within 4 s after that window closed. Anything else is a violation.
CONSEQUENCES = {
    "fold-in-e04-e06-runs:6": {"monitoring_set_active"},                                   # the run became Active: the live cells start
    "fold-in-e04-e06-runs:8": {"monitoring_set_active", "run_quick_look", "shell_log"},    # the run finalized: stop the cells, read the file
}


def analyze_commands(S: Session, steps: list[dict], ck: Check) -> dict:
    ev = S.events
    begin = next((i for i, e in enumerate(ev) if e["k"] == "mark" and e["label"] == "tour-begin"), None)
    end = next((i for i, e in enumerate(ev) if e["k"] == "mark" and e["label"] == "tour-end"), len(ev))
    if begin is None:
        ck.ok("tour window found in the WebSocket log", False)
        return {}
    op, last_op, last_end = None, None, 0.0
    cur = "(starting the tour)"
    per: dict[str, dict] = collections.OrderedDict()
    violations, op_cmds, timeline = [], collections.defaultdict(collections.Counter), []
    t_begin = ev[begin]["t"]
    all_tour, all_op = collections.Counter(), collections.Counter()
    for e in ev[begin:end]:
        if e["k"] == "mark":
            if e["label"].startswith("op-begin:"):
                op = e["label"][9:]
            elif e["label"].startswith("op-end:"):
                last_op, last_end, op = op, e["t"], None
        elif e["k"] == "pop" and e["key"]:
            cur = e["key"]
        elif e["k"] == "send":
            if not is_read(e["cmd"]):
                timeline.append({"t_ms": round(e["t"] - t_begin), "cmd": e["cmd"], "window": op, "step": cur, "args": e.get("args", "")[:100]})
            st = per.setdefault(cur, {"tour": collections.Counter(), "operator": collections.Counter()})
            conseq = (op is None and last_op in CONSEQUENCES and e["cmd"] in CONSEQUENCES[last_op] and e["t"] - last_end <= 4000)
            if conseq:
                timeline[-1]["window"] = f"consequence of {last_op}" if timeline and timeline[-1]["cmd"] == e["cmd"] else None
                st["operator"][e["cmd"] + " (app reaction)"] += 1
                all_op[e["cmd"] + " (app reaction)"] += 1
            elif op:
                st["operator"][e["cmd"]] += 1
                all_op[e["cmd"]] += 1
                op_cmds[op][e["cmd"]] += 1
            else:
                st["tour"][e["cmd"]] += 1
                all_tour[e["cmd"]] += 1
                if not is_read(e["cmd"]):
                    violations.append({"step": cur, "cmd": e["cmd"], "args": e.get("args", "")[:120]})
    ck.ok("the tour sent no command: every command outside the operator's own control use is a read or a poll", not violations,
          f"{len(violations)} violation(s): {violations[:3]}" if violations else f"{sum(all_tour.values())} commands, {len(all_tour)} distinct, all polls/reads")
    # non-poll commands inside operator windows must be the ones the control is expected to send
    stretches = []
    for key, st in per.items():
        stretches.append({"step": key, "tour_commands": dict(st["tour"]), "operator_commands": dict(st["operator"])})
    return {"non_read_timeline": timeline, "violations": violations, "tour_commands": dict(all_tour), "operator_commands": dict(all_op),
            "operator_non_poll": {k: sorted(c for c in v if not is_read(c)) for k, v in op_cmds.items()},
            "stretches": stretches, "non_poll_in_tour_stretches": sorted(c for c in all_tour if not is_read(c))}


# ----------------------------------------------------------------------------------------------------------------------
# Scenarios
# ----------------------------------------------------------------------------------------------------------------------
RPC_JS = r"""
async (url) => {
  const ws = new WebSocket(url);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = () => rej('open failed'); });
  const pending = new Map(); let next = 1;
  ws.onmessage = (m) => { if (typeof m.data !== 'string') return; const j = JSON.parse(m.data);
    if (typeof j.request_id === 'number' && pending.has(j.request_id)) { pending.get(j.request_id)(j); pending.delete(j.request_id); } };
  window.__rpc = (cmd, args) => new Promise((res) => { const id = next++; pending.set(id, res); ws.send(JSON.stringify({request_id: id, cmd, args: args ?? {}})); });
  window.__rpcClose = () => ws.close();
  return true;
}
"""


def seed_i02(browser, server, fixtures: Path) -> dict:
    """What the Create recipe form needs to exist on the instrument, put there through the same commands the headless fold_in_e2e uses
    (setup only, in a throwaway connection that is closed before the operator's browser connects): a local method profile and a
    saved analysis plan on the population fixture. The operator then types their ids into the form."""
    ctx = browser.new_context(viewport=VIEWPORT)
    page = ctx.new_page()
    page.route(server.url + "/seed", lambda r: r.fulfill(body="<html><body>seed</body></html>", content_type="text/html"))
    page.goto(server.url + "/seed")
    page.evaluate(RPC_JS, server.url.replace("http", "ws") + "/ws")

    def rpc(cmd, args=None):
        reply = page.evaluate("([c, a]) => window.__rpc(c, a)", [cmd, args or {}])
        if "error" in reply:
            raise RuntimeError(f"seed {cmd}: {json.dumps(reply)[:400]}")
        return reply["ok"]

    base = server.data / "profiles"
    base.mkdir(parents=True, exist_ok=True)
    if rpc("is_initialized") is not True:
        rpc("init", {"dataDir": ""})
    cfg = rpc("fetch_processing_config_json")
    doc = cfg if isinstance(cfg, str) else json.dumps(cfg)
    created = rpc("profile_command", {"base": str(base), "request": json.dumps({"operation": "create", "name": "tour-method", "document_json": doc})})
    if created.get("ok") is not True:
        raise RuntimeError(f"seed profile create: {json.dumps(created)[:300]}")
    prof = rpc("profile_command", {"base": str(base), "request": json.dumps({"operation": "read", "name": "tour-method"})})
    revision = prof["profile"]["revision"]
    path = str(fixtures / "population-6000.h5")
    source = {"kind": "stored", "path": path}
    opened = rpc("load_recording", {"filePath": path})  # analysis needs a recording open; it is closed again below
    if opened.get("ok") is not True:
        raise RuntimeError(f"seed load_recording: {json.dumps(opened)[:300]}")
    snap = rpc("analysis_read", {"request": {"action": "snapshot", "source": source}})
    binding = {k: snap["source"][k] for k in ("fingerprint", "sourceKey", "parameters", "scope")}
    graph = {"id": "tour-plan", "revision": "draft", "output": "all",
             "nodes": [{"id": "all", "parent": "", "kind": "interval", "feature": "area", "minimum": None, "maximum": None}]}
    saved = rpc("analysis_write", {"request": {"action": "save", "planId": "tour-plan", "expectedRevision": None, "commandId": "save-tour-plan-1",
                                               "graph": graph, "feature": "area", "source": source, "sourceBinding": binding}})
    rpc("close_review")
    page.evaluate("() => window.__rpcClose()")
    ctx.close()
    log(f"    seeded: method profile tour-method @ {revision}; analysis plan tour-plan @ {saved.get('revision')} (on population-6000.h5)")
    return {"profile_base": str(base), "method_id": "tour-method", "method_revision": str(revision), "plan_id": "tour-plan",
            "plan_revision": str(saved.get("revision", "1"))}


def export_feedback(S: Session, ck: Check, cfg, tag: str, expect: dict) -> dict:
    """Open the Tours list after the tour, check it shows the verdict, export the feedback and check the saved file holds the entry."""
    page = S.page
    S.open_list()
    li = page.locator(f'li[data-tour-entry="{expect["tour"]}"]')
    meta = li.locator(".tour-list-meta").inner_text()
    ck.ok("the Tours list shows the stored verdict for the tour", f"your feedback: {expect['verdict']}" in meta, meta)
    link = page.get_by_role("link", name=re.compile(r"Export feedback"))
    label = link.inner_text()
    with page.expect_download(timeout=15000) as dl:
        link.click()
    download = dl.value
    outdir = Path(cfg.out) / "feedback"
    outdir.mkdir(parents=True, exist_ok=True)
    dest = outdir / f"{tag}.json"
    download.save_as(dest)
    data = json.loads(dest.read_text(encoding="utf-8"))
    recs = data.get("records", [])
    mine = [r for r in recs if r.get("tour") == expect["tour"]]
    ck.ok("the exported feedback file holds the entry (tour, verdict, note, time, version)",
          data.get("kind") == "tour-feedback" and len(mine) == 1 and mine[0].get("verdict") == expect["verdict"] and mine[0].get("note") == expect["note"]
          and mine[0].get("at") and mine[0].get("tourVersion"), f"{label!r}: {len(recs)} record(s), file {dest.name}")
    S.shot(f"{S.scheme}/{expect['tour']}", f"{99:02d}-tours-list-after-feedback", kind="list-after-feedback")
    page.get_by_role("button", name="Close").click()
    return {"file": str(dest.relative_to(Path(cfg.out))), "records": len(recs), "download_name": download.suggested_filename}


def prepare(tour_id: str, S: Session, op: Operator, ck: Check):
    """The operator's own set-up before a tour (never part of the tour): the camera for the tours that need a running one."""
    if tour_id == "fold-in-e02-features":
        # what Align says before any camera session exists: the backend's refusal reason, as the state chip's text
        S.page.locator(".mib-stages__tab", has_text="Align").click()
        S.page.wait_for_timeout(1500)
        chips = S.page.evaluate("() => [...document.querySelectorAll('[data-tour=\"align-state-chip\"]')].map(e => e.innerText.replace(/\\s+/g, ' ').trim())")
        op.observations["state chips with the camera idle"] = chips
        log(f"    observed state chips with the camera idle: {chips}")
        S.shot(f"{S.scheme}/extra", "e02-align-before-the-camera-runs", kind="prep")
        S.page.locator(".mib-stages__tab", has_text="Prepare").click()
        S.page.wait_for_timeout(300)
    if tour_id in ("fold-in-e02-features", "fold-in-e04-e06-runs"):
        S.start_camera()
        S.shot(f"{S.scheme}/extra", f"camera-running-before-{tour_id.split('-')[2]}", kind="prep")
        ck.ok("operator started the mock camera in the classic view (set-up)", True)


def run_tour(cfg, browser, shots: Shots, scheme: str, tour_id: str) -> dict:
    short = tour_id.replace("fold-in-", "")
    name = f"{scheme}-{short}"
    ck = Check(name)
    log(f"=== {scheme} / {tour_id}")
    server = cfg.make_server(name)
    LIVE_SERVERS.append(server)
    result = {"scheme": scheme, "tour": tour_id, "checks": ck, "error": None}
    S = None
    try:
        server.start()
        seeded = {}
        if tour_id == "fold-in-i02-recipes":
            seeded = seed_i02(browser, server, Path(cfg.fixtures))
        S = Session(cfg, browser, shots, scheme, name, server)
        S.checks = ck
        player = TourPlayer(cfg, S, tour_id, server, f"{scheme}/{tour_id}")
        player.op.seeded = seeded
        S.load_app()
        prepare(tour_id, S, player.op, ck)
        S.open_list()
        S.shot(f"{scheme}/{tour_id}", "00-tours-list", kind="list")
        ok_play = player.play(ck)
        S.pull()
        pcount = len(S.dialogs)
        cmds = analyze_commands(S, player.steps, ck)
        ck.ok("no uncaught page errors during the tour", not [c for c in S.console if c.startswith("pageerror")], "; ".join(S.console[:3]))
        fb = None
        if ok_play and getattr(player, "feedback_given", None):
            fb = export_feedback(S, ck, cfg, name, {"tour": tour_id, **player.feedback_given})
        result.update(steps=player.steps, actions=player.actions, commands=cmds, observations=player.op.observations, feedback=fb,
                      feedback_given=getattr(player, "feedback_given", None), dialogs=S.dialogs, shots=S.shots_taken, console=S.console,
                      ws_sent_inpage=sum(1 for e in S.events if e["k"] == "send"), ws_sent_playwright=S.pw_sent, steps_total=player.n_steps)
    except Exception as e:  # noqa: BLE001  (a crashed tour is a failed tour; the rest still runs)
        import traceback
        result["error"] = f"{type(e).__name__}: {str(e)[:400]}"
        ck.ok("the tour ran to its end without an exception", False, result["error"])
        log("    " + traceback.format_exc().replace("\n", "\n    "))
        if S:
            try:
                S.shot(f"{scheme}/{tour_id}", "99-error", kind="error")
            except Exception:  # noqa: BLE001
                pass
            result.setdefault("shots", S.shots_taken)
    finally:
        if S:
            S.close()
        server.stop()
        if server in LIVE_SERVERS:
            LIVE_SERVERS.remove(server)
    result["pass"] = not ck.failed and result["error"] is None
    n_shots = len(result.get("shots", []))
    log(f"--- {name}: {'PASS' if result['pass'] else 'FAIL'}  steps {result.get('steps_total')}  screenshots {n_shots}  checks {len(ck.items)} ({len(ck.failed)} failed)")
    return result


# ----------------------------------------------------------------------------------------------------------------------
# Extra scenarios: the notice and list surfaces, the idle gate (E08), the missing-target recovery
# ----------------------------------------------------------------------------------------------------------------------
def walk_to(S: Session, want: int, ck: Check | None = None) -> dict:
    """Press Next until the popover reads `want of N` (a plain step); returns the popover facts."""
    f = S.popover()
    for _ in range(40):
        m = re.match(r"(\d+) of (\d+)$", f["progress"] or "")
        if m and int(m.group(1)) == want:
            return f
        key = f"{f['progress']}|{f['title']}"
        S.page.locator(".driver-popover-next-btn").click()
        f = S.wait_step_change(key, 20)
    raise RuntimeError(f"could not walk to step {want}")


def close_tour(S: Session):
    S.page.keyboard.press("Escape")
    S.page.wait_for_function("() => !document.querySelector('.driver-popover')", timeout=8000)
    S.page.wait_for_timeout(300)


def scenario_surfaces(cfg, browser, shots: Shots, scheme: str) -> dict:
    """The "what's new" notice, the Tours list (from the notice and from the classic Help menu), dismissal, and the quiet rule."""
    name = f"{scheme}-surfaces"
    ck = Check(name)
    log(f"=== {scheme} / surfaces: notice, Tours list, Help menu, quiet while the camera acquires")
    server = cfg.make_server(name)
    LIVE_SERVERS.append(server)
    res = {"scheme": scheme, "name": "surfaces", "checks": ck, "shots": []}
    S = None
    try:
        server.start()
        d = f"{scheme}/extra"
        S = Session(cfg, browser, shots, scheme, name, server)
        S.checks = ck
        S.load_app()
        S.page.wait_for_timeout(2500)
        notice = S.page.locator('[data-testid="tour-notice"]')
        ck.ok("the what's new notice is shown on a fresh browser and names the new tours", notice.count() == 1 and "6 new" in notice.inner_text(), notice.inner_text() if notice.count() else "absent")
        ck.ok("nothing starts a tour by itself (no popover after 4 s on the page)", S.page.locator(".driver-popover").count() == 0)
        S.shot(d, "00-whats-new-notice", kind="notice")
        notice.get_by_role("button", name="take the tour").click()
        S.page.locator('[role=dialog][aria-label="Tours and what\'s new"]').wait_for(timeout=8000)
        S.page.wait_for_timeout(300)
        ck.ok("'take the tour' opens the Tours list with all six tours, each marked new, and starts no tour",
              S.page.locator("li[data-tour-entry]").count() == 6 and S.page.locator(".tour-new-badge").count() == 6 and S.page.locator(".driver-popover").count() == 0,
              f"{S.page.locator('li[data-tour-entry]').count()} entries, {S.page.locator('.tour-new-badge').count()} new badges")
        S.shot(d, "01-tours-list-with-new-badges", kind="list")
        S.page.get_by_role("button", name="Close").click()
        S.page.wait_for_timeout(300)
        ck.ok("once the list was opened the notice is gone (every tour counts as seen)", S.page.locator('[data-testid="tour-notice"]').count() == 0)
        # the classic shell's Help menu
        S.page.get_by_role("button", name="Classic view").click()
        S.page.get_by_role("button", name="Help", exact=True).click()
        S.page.get_by_role("menuitem", name="Tours and what's new").click()
        S.page.locator('[role=dialog][aria-label="Tours and what\'s new"]').wait_for(timeout=8000)
        S.page.wait_for_timeout(300)
        ck.ok("Help > Tours and what's new opens the same list in the classic shell", S.page.locator("li[data-tour-entry]").count() == 6)
        S.shot(d, "02-tours-list-from-the-help-menu", kind="list-help")
        S.page.get_by_role("button", name="Close").click()
        S.page.get_by_role("button", name="Workspace preview").click()
        S.pull()
        starts = [e for e in S.events if e["k"] == "send" and not is_read(e["cmd"]) and e["cmd"] not in LOAD_TIME_ONLY]
        ck.ok("opening the notice and the list sent no command beyond the app's own load-time ones", not starts, str(sorted({e['cmd'] for e in starts})))
        res["shots"] += S.shots_taken
        S.close()
        # dismissal is remembered per browser
        S = Session(cfg, browser, shots, scheme, name, server)
        S.checks = ck
        S.load_app()
        S.page.get_by_role("button", name="Dismiss").click()
        S.page.reload()
        S.page.wait_for_function("() => /backend: ready/.test(document.body.innerText)", timeout=30000)
        S.page.wait_for_timeout(1800)
        ck.ok("a dismissed notice stays dismissed after a reload (stored in this browser)", S.page.locator('[data-testid="tour-notice"]').count() == 0)
        res["shots"] += S.shots_taken
        S.close()
        # quiet while the camera acquires
        S = Session(cfg, browser, shots, scheme, name, server)
        S.checks = ck
        S.load_app()
        ck.ok("(fresh browser) the notice is shown while the camera is idle", S.page.locator('[data-testid="tour-notice"]').count() == 1)
        S.start_camera()
        S.page.wait_for_timeout(500)
        ck.ok("the notice is hidden while the camera acquires (quiet rule)", S.page.locator('[data-testid="tour-notice"]').count() == 0)
        S.shot(d, "03-notice-hidden-while-the-camera-acquires", kind="notice-quiet")
    except Exception as e:  # noqa: BLE001
        import traceback
        ck.ok("the scenario ran without an exception", False, f"{type(e).__name__}: {str(e)[:300]}")
        log("    " + traceback.format_exc().replace("\n", "\n    "))
    finally:
        if S:
            res["shots"] += S.shots_taken
            S.close()
        server.stop()
        LIVE_SERVERS.remove(server) if server in LIVE_SERVERS else None
    res["pass"] = not ck.failed
    log(f"--- {name}: {'PASS' if res['pass'] else 'FAIL'}")
    return res


def scenario_idle_gate(cfg, browser, shots: Shots, scheme: str) -> dict:
    """E08: an action step is shown as blocked while the camera acquires, and again while an experiment runs. The control cannot be
    reached through the overlay, nothing is sent, and the tour does not move on by itself."""
    name = f"{scheme}-idle-gate"
    ck = Check(name)
    log(f"=== {scheme} / idle gate: action step blocked while the camera acquires and while an experiment runs")
    server = cfg.make_server(name)
    LIVE_SERVERS.append(server)
    res = {"scheme": scheme, "name": "idle-gate", "checks": ck, "shots": [], "observations": {}}
    S = None
    d = f"{scheme}/extra"

    def gate_check(tag: str, expect_reason: str, shot: str):
        S.open_list()
        S.page.locator('li[data-tour-entry="fold-in-e01-discovery"]').get_by_role("button", name="Take the tour").click()
        S.wait_step_change("", 20)
        f = walk_to(S, 7)
        status = f["status"] or ""
        ck.ok(f"[{tag}] the action step 'Press Refresh' is shown as blocked: {expect_reason}", status.startswith(f"Not now: {expect_reason}"), status)
        ck.ok(f"[{tag}] its Next button is disabled and the highlighted control is not interactive", f["next_disabled"] and f["target_no_interaction"] is True,
              f"next_disabled={f['next_disabled']} no_interaction={f['target_no_interaction']}")
        S.shot(d, shot, kind="idle-gate", reason=expect_reason)
        S.pull()
        t0 = S.now_ms()
        err = None
        try:
            S.page.locator('[data-tour="connect-refresh"]').click(timeout=2500)
        except (PlaywrightError, PlaywrightTimeout) as e:
            err = explain(e)
        ck.ok(f"[{tag}] the operator cannot press Refresh through the overlay", err is not None, err or "the click went through")
        S.page.wait_for_timeout(1500)
        S.pull()
        sent = sorted({e["cmd"] for e in S.events if e["k"] == "send" and e["t"] >= t0 and not is_read(e["cmd"])})
        g = S.popover()
        ck.ok(f"[{tag}] nothing was sent and the tour stayed on the blocked step", not sent and g and g["progress"] == f["progress"], f"non-read commands: {sent}")
        S.page.locator(".driver-popover .tour-skip").click()
        g = S.wait_step_change(f"{f['progress']}|{f['title']}", 10)
        ck.ok(f"[{tag}] 'Skip this step to go on' works (the tour goes to step 8)", g["progress"] == "8 of 8", g["progress"])
        close_tour(S)

    try:
        server.start()
        S = Session(cfg, browser, shots, scheme, name, server)
        S.checks = ck
        S.load_app()
        S.start_camera()
        gate_check("camera acquiring", "the camera is acquiring", "04-idle-gate-camera-acquiring")
        # now an experiment: the operator starts a run from the workspace Plan
        S.page.locator(".mib-stages__tab", has_text="Experiment").click()
        S.page.get_by_role("button", name="Plan", exact=True).first.click()
        S.prompt_answers.append(str(server.data / "data" / "runs" / "gate-run.h5"))
        S.page.locator('[data-tour="plan-start-run"]').click(timeout=10000)
        S.page.wait_for_function("() => /Experiment: active|Experiment: starting/i.test(document.body.innerText)", timeout=20000)
        S.page.wait_for_timeout(800)
        gate_check("experiment running", "an experiment is running", "05-idle-gate-experiment-running")
        # stop the run again (Live > Stop & finalize run), so the server ends clean
        S.page.locator(".mib-stages__tab", has_text="Experiment").click()
        S.page.get_by_role("button", name="Live", exact=True).first.click()
        S.page.locator('[data-tour="live-stop-run"]').click(timeout=10000)
        S.page.wait_for_function("() => /Experiment: inactive/i.test(document.body.innerText)", timeout=30000)
        ck.ok("the operator stopped the run again and the experiment reads inactive", True)
    except Exception as e:  # noqa: BLE001
        import traceback
        ck.ok("the scenario ran without an exception", False, f"{type(e).__name__}: {str(e)[:300]}")
        log("    " + traceback.format_exc().replace("\n", "\n    "))
        if S:
            try:
                S.shot(d, "99-idle-gate-error", kind="error")
            except Exception:  # noqa: BLE001
                pass
    finally:
        if S:
            res["shots"] = S.shots_taken
            S.close()
        server.stop()
        LIVE_SERVERS.remove(server) if server in LIVE_SERVERS else None
    res["pass"] = not ck.failed
    log(f"--- {name}: {'PASS' if res['pass'] else 'FAIL'}")
    return res


def scenario_missing_target(cfg, browser, shots: Shots, scheme: str) -> dict:
    """A step whose target is absent says why and offers Skip and Restart. The `data-tour` attribute of the next step's element is
    removed before it, so the runner cannot find it; Skip, Back and Restart are each used."""
    name = f"{scheme}-missing-target"
    ck = Check(name)
    log(f"=== {scheme} / missing-target recovery (E01 step 6, connect-devices, attribute removed)")
    server = cfg.make_server(name)
    LIVE_SERVERS.append(server)
    res = {"scheme": scheme, "name": "missing-target", "checks": ck, "shots": []}
    S = None
    d = f"{scheme}/extra"
    try:
        server.start()
        S = Session(cfg, browser, shots, scheme, name, server)
        S.checks = ck
        S.load_app()
        S.open_list()
        S.page.locator('li[data-tour-entry="fold-in-e01-discovery"]').get_by_role("button", name="Take the tour").click()
        S.wait_step_change("", 20)
        f = walk_to(S, 5)
        removed = S.page.evaluate("() => { const e = document.querySelector('[data-tour=\"connect-devices\"]'); if (!e) return false; e.removeAttribute('data-tour'); return true; }")
        ck.ok("the data-tour attribute of the next step's element (connect-devices) was removed before its step", removed)
        key = f"{f['progress']}|{f['title']}"
        S.page.locator(".driver-popover-next-btn").click()
        f = S.wait_step_change(key, 15)
        ck.ok("the step says it is not on screen and why, and does not point at nothing",
              f["title"].startswith("Not on screen") and "not shown on this instrument" in f["description"] and f["target"] is None, f"{f['title']!r}: {f['description'][:140]!r}")
        ck.ok("it offers Skip and Restart (and Back/Next)", "Skip" in f["buttons"] and "Restart" in f["buttons"], str(f["buttons"]))
        S.shot(d, "06-missing-target-says-why", kind="missing", progress=f["progress"])
        # Skip
        S.page.locator(".driver-popover .tour-skip").click()
        g = S.wait_step_change(f"{f['progress']}|{f['title']}", 10)
        ck.ok("Skip goes on to the next step (7 of 8, the Refresh action)", g["progress"] == "7 of 8" and g["status"] is not None, f"{g['progress']} {g['title']!r}")
        S.shot(d, "07-missing-target-skip-went-on", kind="missing-skip", progress=g["progress"])
        # Back to the missing step, then Restart
        S.page.locator(".driver-popover-prev-btn").click()
        f2 = S.wait_step_change(f"{g['progress']}|{g['title']}", 10)
        ck.ok("Back returns to the missing step (still absent, same explanation)", f2["title"].startswith("Not on screen") and f2["progress"] == "6 of 8", f"{f2['progress']} {f2['title']!r}")
        S.page.locator(".driver-popover .tour-restart").click()
        h = S.wait_step_change(f"{f2['progress']}|{f2['title']}", 10)
        ck.ok("Restart goes back to step 1 (1 of 8)", h["progress"] == "1 of 8", f"{h['progress']} {h['title']!r}")
        S.shot(d, "08-missing-target-restart-went-to-step-1", kind="missing-restart", progress=h["progress"])
        close_tour(S)
        S.pull()
        sent = sorted({e["cmd"] for e in S.events if e["k"] == "send" and not is_read(e["cmd"]) and e["cmd"] not in LOAD_TIME_ONLY})
        ck.ok("the missing-target recovery sent no command", not sent, str(sent))
    except Exception as e:  # noqa: BLE001
        import traceback
        ck.ok("the scenario ran without an exception", False, f"{type(e).__name__}: {str(e)[:300]}")
        log("    " + traceback.format_exc().replace("\n", "\n    "))
        if S:
            try:
                S.shot(d, "99-missing-target-error", kind="error")
            except Exception:  # noqa: BLE001
                pass
    finally:
        if S:
            res["shots"] = S.shots_taken
            S.close()
        server.stop()
        LIVE_SERVERS.remove(server) if server in LIVE_SERVERS else None
    res["pass"] = not ck.failed
    log(f"--- {name}: {'PASS' if res['pass'] else 'FAIL'}")
    return res


# ----------------------------------------------------------------------------------------------------------------------
# Contact sheet
# ----------------------------------------------------------------------------------------------------------------------
def build_contact_sheet(cfg, browser, results: list[dict], extras: list[dict], schemes: list[str]):
    """contact-sheet.html (thumbnails grouped by scheme and tour, with titles) and contact-sheet-<scheme>.png (the page rendered in
    Chromium, full page, downscaled)."""
    out = Path(cfg.out)
    esc = html.escape
    parts = ["<!doctype html><html lang=en><head><meta charset=utf-8><title>MIB Studio guided tours: contact sheet</title><style>",
             "body{font:13px/1.4 system-ui,sans-serif;margin:18px;background:#fff;color:#16181c}h1{font-size:20px;margin:0 0 4px}h2{font-size:17px;margin:26px 0 4px;border-bottom:2px solid #16181c}",
             "h3{font-size:14px;margin:18px 0 2px}p.meta{margin:0 0 6px;color:#555;font-size:12px}.grid{display:grid;grid-template-columns:repeat(5,300px);gap:10px 10px}",
             "figure{margin:0}figure img{display:block;width:300px;height:auto;border:1px solid #9aa0a8}figcaption{font-size:11px;padding-top:2px}",
             "figcaption b{font-weight:600}.k{color:#555}.fail{color:#b00020;font-weight:600}nav a{margin-right:12px}</style></head><body>",
             "<h1>MIB Studio guided tours: contact sheet</h1>",
             f"<p class=meta>Browser drive {EVIDENCE_DATE}: headless Chromium {VIEWPORT['width']}x{VIEWPORT['height']} against the real yofo-studio-server with the mock camera. "
             "Every step of every tour, light and dark. Open a thumbnail's file for the full-size image.</p><nav>"]
    parts.append(" ".join(f'<a href="#{s}">{s}</a>' for s in schemes) + "</nav>")
    for scheme in schemes:
        parts.append(f'<section class="scheme" data-scheme="{scheme}" id="{scheme}"><h2>{scheme.capitalize()} colour scheme</h2>')
        for r in [x for x in results if x["scheme"] == scheme]:
            n = len(r.get("shots", []))
            parts.append(f"<h3>{esc(r['tour'])} <span class=k>({'PASS' if r['pass'] else '<span class=fail>FAIL</span>'}, {r.get('steps_total')} steps, {n} screenshots)</span></h3><div class=grid>")
            for sh in r.get("shots", []):
                cap = Path(sh["path"]).stem
                title = sh.get("title") or cap
                kind = sh.get("kind", "")
                parts.append(f'<figure><a href="{esc(sh["path"])}"><img loading=eager src="{esc(sh["path"])}" alt="{esc(cap)}"></a>'
                             f'<figcaption><b>{esc(cap.split("-")[0])}</b> {esc(title)} <span class=k>{esc(kind)}</span></figcaption></figure>')
            parts.append("</div>")
        shots_x = [(x["name"], sh) for x in extras if x["scheme"] == scheme for sh in x.get("shots", [])]
        if shots_x:
            parts.append("<h3>Extra shots: notice, list, idle gate, missing-target recovery</h3><div class=grid>")
            for name, sh in shots_x:
                cap = Path(sh["path"]).stem
                parts.append(f'<figure><a href="{esc(sh["path"])}"><img loading=eager src="{esc(sh["path"])}" alt="{esc(cap)}"></a>'
                             f'<figcaption><b>{esc(name)}</b> {esc(cap)}</figcaption></figure>')
            parts.append("</div>")
        parts.append("</section>")
    parts.append("<script>const h=location.hash.slice(1);if(h)document.querySelectorAll('section.scheme').forEach(s=>{s.hidden=s.dataset.scheme!==h});"
                 "document.querySelector('nav').hidden=!!h;</script></body></html>")
    (out / "contact-sheet.html").write_text("\n".join(parts), encoding="utf-8")
    for scheme in schemes:
        page = browser.new_page(viewport={"width": 1620, "height": 1000})
        try:
            page.goto((out / "contact-sheet.html").as_uri() + f"#{scheme}")
            page.wait_for_function("() => [...document.images].every(i => i.complete)", timeout=60000)
            page.wait_for_timeout(500)
            png = page.screenshot(full_page=True)
        finally:
            page.close()
        dest = out / f"contact-sheet-{scheme}.png"
        if Image is not None:
            img = Image.open(io.BytesIO(png)).convert("RGB")
            w = 1215  # three quarters of the rendered width
            img = img.resize((w, round(img.height * w / img.width)), Image.Resampling.LANCZOS)
            img = img.quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
            img.save(dest, optimize=True)
        else:
            dest.write_bytes(png)
        log(f"contact sheet {dest.name}: {dest.stat().st_size / 1e6:.2f} MB")


# ----------------------------------------------------------------------------------------------------------------------
# main
# ----------------------------------------------------------------------------------------------------------------------
def parse_args():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0], formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("--server", default=str(REPO / "crates/mib-bridge-server/target/debug/yofo-studio-server"), help="the yofo-studio-server binary")
    ap.add_argument("--dist", default=str(REPO / "desktop/dist"), help="the built UI (desktop/dist)")
    ap.add_argument("--fixtures", default=str(HANDOVER / "fixtures"), help="directory with population-6000.h5 and experiment-14.h5")
    ap.add_argument("--mock-dir", default=str(REPO / "data/mock_frames"), help="the mock camera's frame folder")
    ap.add_argument("--run-data", default=str(HANDOVER / "run-data"), help="parent of the fresh per-run data directories")
    ap.add_argument("--out", default=str(REPO / "docs/evidence/2026-11-03-fold-in-tours"), help="output directory")
    ap.add_argument("--schemes", default="light,dark")
    ap.add_argument("--tours", default=",".join(TOURS), help="comma-separated tour ids")
    ap.add_argument("--port", type=int, default=0, help="server port (default: a free one for each server)")
    ap.add_argument("--url", default="", help="use this running server instead of starting one per tour (debugging)")
    ap.add_argument("--no-extras", action="store_true", help="skip the surface, idle-gate and missing-target scenarios")
    ap.add_argument("--no-quantize", action="store_true", help="keep Chromium's PNGs as they are")
    ap.add_argument("--headed", action="store_true", help="show the browser (debugging)")
    return ap.parse_args()


def main() -> int:
    global LOG_FILE
    cfg = parse_args()
    cfg.out = Path(cfg.out)
    cfg.out.mkdir(parents=True, exist_ok=True)
    cfg.mock_dir = Path(cfg.mock_dir)
    cfg.run_data = Path(cfg.run_data)
    cfg.run_data.mkdir(parents=True, exist_ok=True)
    cfg.make_server = (lambda name: ExternalServer(cfg.url)) if cfg.url else (lambda name: Server(cfg, name))
    schemes = [s for s in cfg.schemes.split(",") if s]
    tours = [t for t in cfg.tours.split(",") if t]
    for t in tours:
        if t not in TOURS:
            sys.exit(f"unknown tour {t}; known: {', '.join(TOURS)}")
    for need in (cfg.server, cfg.dist, Path(cfg.fixtures) / "population-6000.h5"):
        if not cfg.url and not Path(need).exists():
            sys.exit(f"missing: {need}")
    LOG_FILE = open(cfg.out / ("drive-log.txt" if tours == TOURS and schemes == ["light", "dark"] else "drive-log-partial.txt"), "w", encoding="utf-8")
    log(f"MIB Studio tour drive {EVIDENCE_DATE}: schemes {schemes}, tours {len(tours)}, server {cfg.server}, dist {cfg.dist}")
    log(f"fixtures {cfg.fixtures}; mock frames {cfg.mock_dir}; out {cfg.out}; PIL {'yes' if Image else 'no'}")
    shots = Shots(cfg.out, not cfg.no_quantize)
    results: list[dict] = []
    with sync_playwright() as pw:
        browser = pw.chromium.launch(args=["--no-sandbox"], headless=not cfg.headed)
        log(f"chromium {browser.version}")
        extras: list[dict] = []
        for scheme in schemes:
            for tid in tours:
                results.append(run_tour(cfg, browser, shots, scheme, tid))
            if not cfg.no_extras:
                for scenario in (scenario_surfaces, scenario_idle_gate, scenario_missing_target):
                    extras.append(scenario(cfg, browser, shots, scheme))
        build_contact_sheet(cfg, browser, results, extras, schemes)
        browser.close()
    return finish(cfg, results, shots, schemes, tours, extras)


def jsonable(r: dict) -> dict:
    out = {k: v for k, v in r.items() if k != "checks"}
    ck = r["checks"]
    out["checks"] = ck.items
    out["checks_failed"] = len(ck.failed)
    for st in out.get("steps", []):
        st.pop("shown_t", None)
    return out


def finish(cfg, results, shots, schemes, tours, extras=None) -> int:
    log("")
    log("SUMMARY")
    ok = True
    for r in results:
        ok &= r["pass"]
        n = len([s for s in r.get("steps", []) if s["kind"] != "feedback"])
        log(f"  {r['scheme']:5} {r['tour']:30} {'PASS' if r['pass'] else 'FAIL'}  steps {r.get('steps_total')}  shots {len(r.get('shots', []))}  "
            f"checks {len(r['checks'].items)} ({len(r['checks'].failed)} failed)")
    for x in extras or []:
        ok &= x["pass"]
        log(f"  {x['scheme']:5} {x['name']:30} {'PASS' if x['pass'] else 'FAIL'}  shots {len(x.get('shots', []))}  checks {len(x['checks'].items)} ({len(x['checks'].failed)} failed)")
    manifest = {
        "kind": "mib-studio-tour-drive", "date": EVIDENCE_DATE, "generated": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "viewport": VIEWPORT, "schemes": schemes, "tours": tours, "hold_seconds": HOLD_S,
        "poll_allowlist": POLL.pattern, "view_read_allowlist": VIEW_READ.pattern,
        "runs": [jsonable(r) for r in results], "extras": [jsonable(x) for x in (extras or [])],
        "png_count": shots.count, "png_bytes": shots.bytes, "all_passed": bool(ok),
    }
    (cfg.out / "manifest.json").write_text(json.dumps(manifest, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    log(f"PNGs {shots.count}, {shots.bytes / 1e6:.2f} MB; manifest {cfg.out / 'manifest.json'}")
    log("ALL CHECKS PASSED" if ok else "FAILED: see the [FAIL] lines above")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
