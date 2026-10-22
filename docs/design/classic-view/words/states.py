"""O1b summary-first Prepare: the content of the three states, shared by the Penpot
composer (compose.py) and the kit-built HTML prototype (app.jsx). Illustrative data.
Words follow desktop/src/workspace/prepareModel.ts (gateText "State · reason", GATE_LABELS,
GATE_REMEDIATIONS, startRefusalText, READINESS_FOOTNOTE), calibrationModel.ts and the backend gate
reasons/remediations in ExperimentCoordinator.cpp. A passing gate has no reason, so it reads "Pass".

Gavin's decisions on O1b (2026-10-22):
1. Gates a running camera or Align resolves (camera.session, camera.deliveryMode, camera.geometry,
   telemetry.transportLoss, processing.roi, processing.background) do not count as issues in Prepare.
   They show as one neutral, always-visible "Next · Align" line; Start still refuses until they pass.
   A one-line refusal note sits beside Confirm only when a gate (counting or deferred) would refuse Start.
2. A mock camera chosen on purpose reads "Ready · simulated camera"; its camera.source warning does not
   count and does not open the Camera row.
3. Confirm stays enabled (advisory, per tab, checklist-driven).
4. "How Prepare reads readiness" lives inside All checks; the first page has no side card."""
import json

FOOTNOTE = "Unknown is never Pass. Unsupported never blocks. Start re-evaluates every gate with the chosen file."
FIX_SOURCE = "Fix: select the mock camera explicitly, or install/connect the hardware"
FIX_CAL = "Fix: run the calibration method on this setup (Experiment · Plan), then Apply its record"
NEXT_FIX = "Next · Align: not counted in Prepare; Start waits for it"
SIM_NOTE = "Chosen on purpose: expected for development/tests; not a hardware run. Not counted."

HOW = {"title": "How Prepare reads readiness",
       "lines": ["The backend evaluates every gate; this page only shows them, read without a destination: no file is written while you look.",
                 FOOTNOTE,
                 "Gates that a running camera or Align resolves are listed under Next · Align and are not counted here; Start still waits for them.",
                 "A mock camera chosen on purpose reads Ready · simulated camera. A row with a counted issue opens by itself, with its Fix.",
                 "Confirm follows the hardware checklist, is kept for this browser tab, and Start does not require it."]}

NEXT_ALIGN = {"label": "Next · Align", "text": "start the camera, set ROI and background · Start waits for these 6 gates"}

def common(source):
    return {
        "brand": "YOFO Studio", "tag": "Workspace preview",
        "context": [{"label": "Source", "value": source}, {"label": "Method", "value": "Cell mechanics / v3"}],
        "headerAction": "Classic view",
        "stages": ["Prepare", "Align", "Experiment", "Review"],
        "band": {"left": "PROPOSED WIREFRAME  /  ILLUSTRATIVE DATA",
                 "right": "O1b · summary-first Prepare · approved by Gavin 2026-10-22 · composed from O1 + C · boards · kit gap: disclosure row (#715)"},
        "toolbar": {"buttons": [{"label": "Overview", "variant": "selected"}, {"label": "Refresh", "variant": "secondary"}],
                    "title": "Hardware preflight"},
    }

def gate(label, gid, tone, text, fix=None):
    d = {"label": label, "id": gid, "tone": tone, "text": text}
    if fix: d["fix"] = fix
    return d

def fact(label, value, tone=None, gid=None, chip=False, fix=None):
    d = {"label": label, "value": value}
    if tone: d["tone"] = tone
    if gid: d["id"] = gid
    if chip: d["chip"] = True
    if fix: d["fix"] = fix
    return d

P = "confirmed"
N = "neutral"
# Deferred to Align (decision 1): neutral tone, real backend words.
SESSION = gate("Camera session", "camera.session", N, "Fail · camera is idle")
DELIVERY = gate("Frame delivery", "camera.deliveryMode", N, "Unavailable · delivery mode is confirmed only by a running camera")
GEOMETRY = gate("Frame size", "camera.geometry", N, "Unavailable · no frame has been received yet")
LOSS = gate("Frame loss reporting", "telemetry.transportLoss", N, "Unavailable · no active session")
ROI = gate("Region of interest", "processing.roi", N, "Warn · no ROI set; full frame is processed")
BACKGROUND = gate("Background image", "processing.background", N, "Warn · no background image set; detection uses frame thresholds only")
SOURCE_SIM = gate("Camera source", "camera.source", N, "Warn · simulated camera source selected")
SOURCE_FAIL = gate("Camera source", "camera.source", "blocked",
                   "Fail · requested egrabber camera is unavailable; no eGrabber driver on this computer", fix=FIX_SOURCE)
CAL_OK = gate("Calibration record", "calibration.record", P, "Pass")
CAL_WARN = gate("Calibration record", "calibration.record", "warn", "Warn · record 31 h old · policy: warn", fix=FIX_CAL)

def gates(source, cal):
    return [SESSION, source, DELIVERY, GEOMETRY, LOSS,
            gate("Processing core", "processing.core", P, "Pass"),
            gate("Sort trigger output", "trigger.output", N, "Not required"),
            cal, ROI, BACKGROUND,
            gate("Processing settings", "processing.config", P, "Pass"),
            gate("Background calibration", "processing.backgroundCalibration", P, "Pass"),
            gate("Method revision", "method.revision", P, "Pass"),
            gate("Wall clock", "clock.wall", P, "Pass"),
            gate("Running experiment", "lifecycle.experiment", P, "Pass"),
            gate("Previous run fault", "lifecycle.fault", P, "Pass"),
            gate("Recording file", "storage.hdf5", P, "Pass"),
            gate("Recording memory budget", "storage.buffer", P, "Pass"),
            gate("Raw recording", "lifecycle.recording", P, "Pass"),
            gate("Destination file", "storage.output", "unknown", "Unavailable · no output path chosen yet")]

def gfact(g, fix=None):
    return fact(g["label"], g["text"], g["tone"], gid=g["id"], chip=True, fix=fix if fix is not None else g.get("fix"))

def camera_facts(source_gate, source_fix):
    return [fact("Camera · required", "Pass · Mock camera · Connected.", P),
            fact("Processing core / trust · required", "Pass · 0.3.2 · Trusted core is active and pinned", P),
            fact("Capture stream", "Not required · not started · Starts when the camera runs."),
            fact("Trigger / sorter", "Not required · Optional — trigger not attached."),
            gfact(source_gate, source_fix),
            gfact(SESSION, NEXT_FIX), gfact(DELIVERY, NEXT_FIX), gfact(GEOMETRY, NEXT_FIX), gfact(LOSS, NEXT_FIX),
            fact("Processing core", "Pass", P, gid="processing.core", chip=True)]

ROW_OPTIONAL = {"key": "optional", "quiet": True, "title": "Optional · not connected",
                "summary": "Focus stage, sample pump, sheath pump, trigger / sorter",
                "chip": {"tone": N, "text": "Not required · 4"}, "expanded": False,
                "facts": [fact("Coarse Z stage", "Not connected"),
                          fact("Autofocus / nanopositioner", "Not required · Optional — not connected."),
                          fact("Sample pump", "Not required · Optional — not connected."),
                          fact("Sheath pump", "Not required · Optional — not connected."),
                          fact("Trigger / sorter", "Not required · Optional — trigger not attached.")],
                "actions": [{"label": "Focus setup", "variant": "secondary"}, {"label": "Pump setup", "variant": "secondary"}]}

ROW_DEST = {"key": "destination", "title": "Data destination",
            "summary": "File chosen and checked at Start · recording file and budget pass",
            "chip": {"tone": "unknown", "text": "Unknown · checked at Start"}, "expanded": False,
            "facts": [fact("Storage destination", "Unknown · Not reported before Start: the backend checks the chosen file when Start is pressed.", "unknown"),
                      fact("Destination file", "Unavailable · no output path chosen yet", "unknown", gid="storage.output", chip=True,
                           fix="Expected in Prepare: the file is chosen at Start. Never counted as an issue."),
                      fact("Recording file", "Pass", P, gid="storage.hdf5", chip=True),
                      fact("Recording memory budget", "Pass", P, gid="storage.buffer", chip=True),
                      fact("Raw recording", "Pass", P, gid="lifecycle.recording", chip=True)]}

ROW_METHOD = {"key": "method", "title": "Method", "summary": "Processing settings, method revision, wall clock, previous run",
              "chip": {"tone": P, "text": "Pass · 6 gates"}, "expanded": False,
              "facts": [fact(l, "Pass", P, gid=i, chip=True) for l, i in [
                  ("Processing settings", "processing.config"), ("Background calibration", "processing.backgroundCalibration"),
                  ("Method revision", "method.revision"), ("Wall clock", "clock.wall"),
                  ("Running experiment", "lifecycle.experiment"), ("Previous run fault", "lifecycle.fault")]]}

ROW_CAL_OK = {"key": "calibration", "title": "Calibration", "summary": "cal-0042 · 0.4886 µm/px · applied 3 h ago",
              "chip": {"tone": P, "text": "Pass · in effect"}, "expanded": False,
              "facts": [gfact(CAL_OK), fact("In effect", "cal-0042 · 0.4886 µm/px · 15.2 µm beads · applied 3 h ago")],
              "actions": [{"label": "Records", "variant": "secondary"}]}

ROW_CAMERA_SIM = {"key": "camera", "title": "Camera and acquisition",
                  "summary": "Mock camera (chosen) · core 0.3.2 trusted · session and frames are checked in Align",
                  "chip": {"tone": P, "text": "Pass · simulated camera"}, "expanded": False,
                  "facts": camera_facts(SOURCE_SIM, SIM_NOTE), "actions": [{"label": "Camera setup", "variant": "secondary"}]}

READY_VERDICT = {
    "title": "Hardware ready · simulated camera",
    "detected": "Mock camera (chosen) · core 0.3.2 trusted · calibration cal-0042 in effect · optional: focus, pumps and trigger not connected",
    "chips": [{"tone": P, "text": "Ready · simulated camera"}],
    "next": NEXT_ALIGN,
    "confirmation": {"label": "Preflight confirmation", "value": "Not confirmed in this browser tab"},
    "action": "Confirm hardware preflight",
    "refusal": "Start is refused until the camera runs in Align.",
}

def all_checks(source, cal, summary, expanded=False):
    return {"summary": summary, "expanded": expanded, "gates": gates(source, cal), "how": HOW,
            "action": {"label": "Read gates again", "variant": "secondary"}}

READY_SUMMARY = "20 backend gates · 11 pass · 6 next in Align · 1 simulated camera · 1 not required · destination checked at Start"

STATES = {
    "ready": {
        "board": "O1b · Prepare · summary-first · all ready",
        "common": common("Mock camera"),
        "pill": {"tone": P, "text": "Ready · simulated camera"},
        "verdict": READY_VERDICT,
        "rows": [ROW_CAMERA_SIM, ROW_CAL_OK, ROW_METHOD, ROW_DEST, ROW_OPTIONAL],
        "allChecks": all_checks(SOURCE_SIM, CAL_OK, READY_SUMMARY),
    },
    "attention": {
        "board": "O1b · Prepare · summary-first · needs attention",
        "common": common("Mock camera (fallback)"),
        "pill": {"tone": "warn", "text": "Ready to confirm · Start blocked: Camera source"},
        "verdict": {
            "title": "Needs attention · 2",
            "detected": "Mock camera (fallback: eGrabber camera unavailable) · core 0.3.2 trusted · calibration cal-0042 · optional: focus, pumps and trigger not connected",
            "chips": [{"tone": "warn", "text": "Ready to confirm · Start blocked: Camera source"},
                      {"tone": "unknown", "text": "1 failed, 1 warning"}],
            "next": NEXT_ALIGN,
            "confirmation": {"label": "Preflight confirmation", "value": "Not confirmed in this browser tab"},
            "action": "Confirm hardware preflight",
            "refusal": "Start is refused until Camera source passes and the camera runs in Align.",
        },
        "rows": [
            {"key": "camera", "title": "Camera and acquisition",
             "summary": "Mock camera (fallback) · core 0.3.2 trusted · requested eGrabber camera unavailable",
             "chip": {"tone": "blocked", "text": "Fail · Camera source"}, "expanded": True, "auto": True,
             "facts": [gfact(SOURCE_FAIL)],
             "more": "Camera and Processing core / trust pass · 4 gates wait for Align · Show all",
             "rest": [f for f in camera_facts(SOURCE_FAIL, None) if f.get("id") != "camera.source"],
             "actions": [{"label": "Camera setup", "variant": "secondary"}]},
            {"key": "calibration", "title": "Calibration", "summary": "cal-0042 · 0.4886 µm/px · applied 31 h ago",
             "chip": {"tone": "warn", "text": "Warn · record 31 h old"}, "expanded": True, "auto": True,
             "facts": [gfact(CAL_WARN), fact("In effect", "cal-0042 · 0.4886 µm/px · 15.2 µm beads · applied 31 h ago")],
             "actions": [{"label": "Records", "variant": "secondary"}]},
            ROW_METHOD, ROW_DEST, ROW_OPTIONAL,
        ],
        "allChecks": all_checks(SOURCE_FAIL, CAL_WARN,
                                "20 backend gates · 1 failed · 1 warning · 10 pass · 6 next in Align · 1 not required · destination checked at Start"),
    },
    "expanded": {
        "board": "O1b · Prepare · summary-first · expanded details",
        "common": common("Mock camera"),
        "pill": {"tone": P, "text": "Ready · simulated camera"},
        "verdict": READY_VERDICT,
        "rows": [dict(ROW_CAMERA_SIM, expanded=True), ROW_CAL_OK, ROW_METHOD, ROW_DEST, ROW_OPTIONAL],
        "allChecks": all_checks(SOURCE_SIM, CAL_OK, READY_SUMMARY, expanded=True),
    },
}

HARDWARE_CARD = {"title": "What was checked",
                 "subtitle": "Auto-detected and selected. Open a row for its checks; a row with a counted issue opens by itself."}

if __name__ == "__main__":
    json.dump({"hardware": HARDWARE_CARD, "states": STATES}, open("/tmp/o1b/states.json", "w"), ensure_ascii=False, indent=1)
    print("ok")
