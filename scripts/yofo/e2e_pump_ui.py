"""End-to-end: YOFO Studio UI (served by yofo-studio-server on the PZ7035 PS) drives the
Tushui peristaltic pump on /dev/ttyPS1 through the Sample pump slot.

  python3 scripts/yofo/e2e_pump_ui.py http://192.168.137.2:8427 TOKEN OUTDIR [RUN_SECONDS]

Needs Python Playwright with Chromium on the host and a yofo-studio-server on the PS; a
mock-camera server leaves the PL alone (docs/integration/tushui-peristaltic-pump.md).

Connect (read-only) -> set 500 uL/min (20 rpm at 25 uL/rev) + Infuse -> Service mode + arm ->
Run -> watch status -> Stop -> restore the as-found rate/direction -> Disconnect.
RUN_SECONDS 0 skips the run (no motion).
"""
import re
import sys
import time
from pathlib import Path

from playwright.sync_api import expect, sync_playwright

base, token, outdir = sys.argv[1], sys.argv[2], Path(sys.argv[3])
run_s = float(sys.argv[4]) if len(sys.argv) > 4 else 3.0
outdir.mkdir(parents=True, exist_ok=True)
log = []


def note(msg):
    line = f"{time.strftime('%H:%M:%S')} {msg}"
    print(line, flush=True)
    log.append(line)


with sync_playwright() as p:
    browser = p.chromium.launch()
    page = browser.new_page(viewport={"width": 1500, "height": 1300})
    page.on("dialog", lambda d: d.accept())
    page.goto(f"{base}/?token={token}")
    sample = page.locator("fieldset", has=page.locator("legend", has_text="Sample pump"))
    status_line = sample.locator("p").first
    expect(status_line).to_contain_text("Disconnected", timeout=30000)
    note("UI connected to the board server; Sample pump slot disconnected")

    sample.get_by_label("Pump model").select_option("1")
    expect(sample.get_by_label(re.compile("System serial port"))).to_have_value("/dev/ttyPS1")
    expect(sample.get_by_label("Device address")).to_have_value("3")
    expect(sample.get_by_label(re.compile("Calibration"))).to_have_value("25")
    sample.get_by_role("button", name="Connect", exact=True).click()
    expect(status_line).to_contain_text("Connected", timeout=15000)
    detail = sample.locator("p").nth(1)
    expect(detail).to_contain_text("Peristaltic", timeout=10000)
    as_found = detail.inner_text()
    note(f"connected: {as_found}")
    page.screenshot(path=str(outdir / "1-connected.png"), full_page=True)
    found_rate = re.search(r"Configured rate ([\d.]+) \(µL/min\)", as_found)
    found_dir = 1 if "Direction Withdraw" in as_found else 0

    def apply_rate(value):
        sample.get_by_label("Flow rate").fill(str(value))
        sample.get_by_role("button", name="Apply rate").click()

    def apply_direction(value):
        sample.get_by_label("Direction").select_option(str(value))
        sample.get_by_role("button", name="Apply direction").click()

    apply_rate(500)
    expect(detail).to_contain_text("Head 20.00 rpm", timeout=10000)
    apply_direction(0)
    expect(detail).to_contain_text("Direction Infuse", timeout=10000)
    note(f"configured: {detail.inner_text()}")

    if run_s > 0:
        page.get_by_role("button", name=re.compile("^Mode: Operator")).click()
        page.get_by_label("Arm one hardware action").check()
        sample.get_by_role("button", name="Run configured pump").click()
        t0 = time.monotonic()
        expect(status_line).to_contain_text("Forward", timeout=5000)
        page.screenshot(path=str(outdir / "2-running.png"), full_page=True)
        while time.monotonic() - t0 < run_s:
            sample.get_by_role("button", name="Read device status").click()
            note(f"running: {status_line.inner_text()} | {detail.inner_text()}")
            time.sleep(0.7)
        sample.get_by_role("button", name="Stop pump").click()
        expect(status_line).to_contain_text("Stopped", timeout=5000)
        note(f"stopped after {time.monotonic() - t0:.1f} s: {detail.inner_text()}")
        page.screenshot(path=str(outdir / "3-stopped.png"), full_page=True)
        page.get_by_role("button", name="Exit to Operator").click()

    # Put the pump back as found.
    if found_rate:
        apply_rate(float(found_rate.group(1)))
    apply_direction(found_dir)
    expect(detail).to_contain_text("Direction " + ("Withdraw" if found_dir else "Infuse"), timeout=10000)
    note(f"restored: {detail.inner_text()}")
    sample.get_by_role("button", name="Disconnect").click()
    expect(status_line).to_contain_text("Disconnected", timeout=10000)
    note("disconnected")
    page.screenshot(path=str(outdir / "4-disconnected.png"), full_page=True)
    browser.close()

(outdir / "e2e.log").write_text("\n".join(log) + "\n")
