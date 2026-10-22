"""Classic · Hardware Preflight · summary-first: content for the two classic boards. Reuses the
approved O1b content (states.py) for the main area and adds the Focus and Fluidics rows, the classic
shell (menu bar, stage tabs with their Next: band, status bar, bridge line) and the redesigned left
column in two variants (idle mock camera; running experiment). Illustrative data; values the backend
reports as unavailable read "—" (never "Unavailable" in a value column)."""
import json
import states as S

P, N = "confirmed", "neutral"
fact = S.fact

# No configuration on the first page (Gavin 2026-10-22): a row shows its checks, the detected facts, a
# Fix line and at most one small "Set up…" action that leaves the page for Classic · Hardware setup.
ROW_FOCUS = {"key": "focus", "title": "Focus", "summary": "Autofocus controller, nanopositioner and Z stage · not connected",
             "chip": {"tone": N, "text": "Not connected · optional"}, "expanded": True,
             "facts": [fact("Autofocus controller", "Not required · Optional — not connected."),
                       fact("Nanopositioner", "Not required · Optional — no endpoint remembered",
                            fix="Fix: optional. To use autofocus, connect the controller in Hardware setup (Set up focus…); nothing on this page edits hardware"),
                       fact("Coarse Z stage", "Not connected")],
             "actions": [{"label": "Set up focus…", "variant": "secondary"}]}
ROW_FLUIDICS = {"key": "fluidics", "title": "Fluidics", "summary": "Sample and sheath pumps · not connected",
                "chip": {"tone": N, "text": "Not connected · optional"}, "expanded": False,
                "facts": [fact("Sample pump", "Not required · Optional — not connected."),
                          fact("Sheath pump", "Not required · Optional — not connected.")],
                "actions": [{"label": "Set up pumps…", "variant": "secondary"}]}
ROW_CAMERA = dict(S.ROW_CAMERA_SIM, summary="Mock camera (chosen) · core 0.3.2 trusted · trigger not attached · session and frames are checked in Align",
                  actions=[{"label": "Set up camera…", "variant": "secondary"}])

MAIN = {
    "verdict": S.READY_VERDICT,
    "rows": [ROW_CAMERA, ROW_FOCUS, ROW_FLUIDICS, S.ROW_CAL_OK, S.ROW_METHOD, S.ROW_DEST],
    "allChecks": S.all_checks(S.SOURCE_SIM, S.CAL_OK, S.READY_SUMMARY),
}

SHELL = {
    "menu": ["File", "Settings", "Help"],
    "menuRight": ["Workspace preview", "Mode: Operator"],
    "stages": [{"name": "Hardware Preflight", "status": "Ready", "dot": P, "active": True},
               {"name": "Camera & Alignment", "status": "Not started", "dot": None},
               {"name": "Experiment", "status": "Not started", "dot": None},
               {"name": "Review", "status": "Not started", "dot": None}],
    "cameraButtons": ["Start Camera", "Stop Camera"],
    "next": {"label": "Next:", "text": "Hardware Preflight — Checks pass — confirm the hardware is ready", "action": "Confirm hardware preflight"},
    "status": [("Profile", "No profile", "unknown"), ("Camera", "Mock camera", P), ("Calibration", "0.4886 µm/px", P),
               ("Status", "Hardware Preflight", P), ("Operator", "—", "unknown"), ("Storage", "—", "unknown"), ("Warnings", "2", "warn")],
    "bridge": {"left": "Bridge ABI: 44 · backend: ready   Log ▸ (2)",
               "right": "Display=0.0 fps | Algo=0.0/s | Valid=0.0/s | Invalid=0.0/s | Camera=idle, 0.0 MB/s | Experiment: inactive"},
}

def row(k, v, tone=None, dim=False):
    return {"k": k, "v": v, "tone": tone, "dim": dim}

# Variant A: idle mock camera (the state of boards 1 and 2).
COLUMN_IDLE = {
    "title": "Left column · idle mock camera",
    "stage": {"chip": {"tone": P, "text": "01 Hardware Preflight · Ready"}, "line": "Mock camera · idle · nothing runs"},
    "sections": [
        {"title": "Camera", "rows": [row("Status", "Idle", dim=True), row("Display rate", "0.0 fps", dim=True), row("Data rate", "0.0 MB/s", dim=True)]},
        {"title": "Processing", "rows": [row("Algo / valid / invalid", "0.0 / 0.0 / 0.0 fps", dim=True), row("px → µm", "0.4886"),
                                         row("Background", "Not set", dim=True)]},
        {"title": "Focus · not connected", "collapsed": True},
        {"title": "Fluidics · not connected", "collapsed": True},
        {"title": "Experiment", "rows": [row("Status", "Inactive", dim=True), row("Elapsed", "—", dim=True),
                                         row("Buffered", "0 valid · 0 invalid", dim=True), row("Saved", "0 valid · 0 invalid", dim=True),
                                         row("Pending save", "0", dim=True), row("Dropped", "0 valid · 0 invalid", dim=True),
                                         row("Failed to save", "0", dim=True), row("Flush", "Idle", dim=True)]},
    ],
}

# Variant B: a running experiment with focus and pumps connected.
COLUMN_RUNNING = {
    "title": "Variant · running experiment",
    "stage": {"chip": {"tone": P, "text": "03 Experiment · Active"}, "line": "Mock camera · running · Exp-0142_03 · sample 3 of 7"},
    "sections": [
        {"title": "Camera", "rows": [row("Status", "Running", P), row("Display rate", "30.0 fps"), row("Data rate", "245.8 MB/s")]},
        {"title": "Processing", "rows": [row("Algo / valid / invalid", "412 / 398 / 14 fps"), row("px → µm", "0.4886"),
                                         row("Background", "Set · generation 3", P)]},
        {"title": "Focus", "rows": [row("Controller", "connected (auto)", P), row("Ring width", "0.512"), row("Voltage", "42.3 V"),
                                    row("Metric age", "18 ms")]},
        {"title": "Fluidics", "rows": [row("Sample pump", "0.040 µL/s · running", P), row("Sheath pump", "0.120 µL/s · running", P)]},
        {"title": "Experiment", "rows": [row("Status", "Active", P), row("Elapsed", "128 s"),
                                         row("Buffered", "1 240 valid · 86 invalid"), row("Saved", "51 902 valid · 1 311 invalid"),
                                         row("Pending save", "1 326"), row("Dropped", "0 valid · 0 invalid", dim=True),
                                         row("Failed to save", "0", dim=True), row("Flush", "Idle", dim=True)]},
    ],
}

# Today's column, for the "before" comparison on board 2 (copied from the screenshot).
COLUMN_TODAY = {
    "title": "Today (for comparison)",
    "sections": [
        {"title": "Display", "rows": [row("FPS:", "0.0")]},
        {"title": "Processing", "rows": [row("Algo FPS:", "0.0"), row("Valid FPS:", "0.0"), row("Invalid FPS:", "0.0"), row("px→µm:", "0.4886")]},
        {"title": "Camera", "rows": [row("Status:", "unconfigured"), row("Display rate:", "0.0 fps"), row("Data rate:", "0.0 MB/s")]},
        {"title": "Autofocus", "rows": [row("Ring width:", "—", dim=True), row("Controller:", "disconnected", dim=True)]},
        {"title": "Experiment", "rows": [row("Status:", "Inactive", dim=True), row("Valid Buffered:", "0"), row("Invalid Buffered:", "0"),
                                         row("Flush Status:", "Idle"), row("Valid Images Saved:", "0"), row("Invalid Images Saved:", "0"),
                                         row("Images Pending Save:", "0"), row("Valid Images Dropped:", "0"), row("Invalid Images Dropped:", "0"),
                                         row("Images Failed to Save:", "0"), row("Elapsed (wall):", "—")]},
        {"title": "Nanopositioner Autofocus", "rows": [row("Endpoint:", "Disconnected", dim=True), row("Voltage:", "—", dim=True), row("Metric age:", "—", dim=True)]},
    ],
}

BOARDS = {
    "classic1": {"name": "Classic · Hardware Preflight · summary-first", "column": "today", "variant": None},
    "classic2": {"name": "Classic · Hardware Preflight · summary-first · redesigned left column", "column": "idle", "variant": "running"},
}

if __name__ == "__main__":
    json.dump({"main": MAIN, "shell": SHELL, "columns": {"idle": COLUMN_IDLE, "running": COLUMN_RUNNING, "today": COLUMN_TODAY},
               "boards": BOARDS, "hardware": S.HARDWARE_CARD}, open("/tmp/o1b/classic_states.json", "w"), ensure_ascii=False, indent=1)
    print("ok")
