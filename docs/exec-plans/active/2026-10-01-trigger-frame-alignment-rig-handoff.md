# Handover: sort pulse ↔ frame alignment — rig work (Euresys + SSG3021X)

Status: active

Date: 2026-10-01. Author: the Linux-container agent session that implemented
the software layer (2026-09-30). Consumer: the agent working on the rig PC
(Windows, Coaxlink + CoaXPress camera, SIGLENT SSG3021X on LAN). Companion
documents: the execution plan
[`2026-09-30-trigger-frame-alignment.md`](2026-09-30-trigger-frame-alignment.md)
(problem, model, decision log), vault notes
`knowledge_map/services/TriggerService.md`,
`knowledge_map/services/RfGeneratorService.md`,
`knowledge_map/camera/ICamera.md`, `knowledge_map/camera/EGrabberCamera.md`,
storage schema `knowledge_map/data-model/HDF5-Storage.md`.

## 0. Scope of this hand-off (read first)

Decided with the user on 2026-10-01:

| Do now | Parked (later, once an oscilloscope is attached) |
|---|---|
| Build the branch on the rig PC (Windows) and run the test suite | Loopback of SSG PULSE OUT into the grabber (§3) — both TTL lines on this rig's breakout are already used |
| Make MIB Studio talk to the SSG3021X over **LAN** (§4) and prove it on the real instrument (§6a) | `EGrabberCamera::setLineEventCallback` (§5) — nothing to stamp until there is a loopback input |
| Record one sorting run and check the new datasets are in the file (§6a) | Loopback/frame-placement evidence (§6) and the scope cross-check |

Do **not** add cables to the rig and do **not** write to the SSG (no
`applySortWindow`, no front-panel changes) unless the user asks.

## 1. Where the work is

| Item | Value |
|---|---|
| Repository | `gavinlouuu-kpt/mib-studio-qt` |
| Branch | `feat/trigger-frame-alignment`, pushed to `origin`; **no PR opened** |
| Base | `develop` at `e1c2bf3` |
| Commits (oldest first) | `52e7821` series identity + `/trigger_events` + `ICamera::setLineEventCallback` + mock loopback · `4d785f8` `RfGeneratorService` (SCPI, LAN/USB), `rf.generator` readiness gate, `rf_generator_*` provenance · `3655a42`, `9ecef5f`, `553a202` this handover, Coaxlink pin tables, annotated rig photos · `726ae19` disabled `rf_generator` block in the bundled default config + empty-address message · then this scope update |
| Verified here | Linux backend-only build; full `ctest -LE network` green except the two pre-existing `scripts.*` tests that need `numpy`; the trigger/pipeline tests also clean under ThreadSanitizer |
| **Not compiled anywhere yet** | `src/backend/services/ScpiTransportVisaWin32.cpp` (Windows-only, NI-VISA loaded at runtime). Not needed on this rig (LAN chosen) but it is in the Windows build — build it once and fix any MSVC nit |

## 2. What the software already does

The data file can now place a sort pulse against the recorded frames:

- `/trigger_events` — one row per sort request from `TriggerService`:
  source `frameIndex`, host grab stamp, object/track ids, host-monotonic
  `requestUs` / `wakeUs` / `fireUs` / `pulseDoneUs`, the real `outcome`, and
  two columns that are **0 until this handover's work lands**:
  `lineEdgeTimestamp` (hardware stamp of the looped-back pulse in the frame
  clock) and `lineEdgeHostUs`.
- `/valid_frames/series_meta` + `series_contiguous` — every multi-image
  member's frame index, camera stamp and host stamp; a series that skipped
  frames is flagged.
- `rf_generator_*` attributes — the SSG3021X readback (trigger mode, trigger
  delay, pulse width, RF on, …) taken at readiness time; the `rf.generator`
  readiness gate blocks a sorting run on a down or mis-armed generator.
- `ICamera::setLineEventCallback(cb)` — a camera backend that can stamp its
  inputs reports `LineEvent{line, rising, timestamp, hostTimestampUs}`;
  `TriggerService` subscribes on bind and pairs rising edges FIFO with the
  fired records. `MockCamera` emulates it; **`EGrabberCamera` does not
  implement it yet — that is the main job below.**

On Coaxlink/Windows the frame timestamp (`BUFFER_INFO_TIMESTAMP`) is µs since
boot = the same clock as `Tools::getTimestamp()` = the same clock the I/O
toolbox stamps events in, so once the edge is captured, `fireUs`,
`lineEdgeTimestamp` and the frames' `timestampNs` are directly comparable.

## 3. Rig wiring — PARKED (loopback deferred until the scope is attached)

Kept for when the loopback resumes; nothing in this section is to be done in
this hand-off. Resolved since it was written: the green/yellow wires on
terminals 24/25 are the **LED** on TTLIO11, so TTLIO11 is **not** available;
re-target to IIN11 or TTLIO21 (see the "Loopback status" note in §4).

Current state on the SSG3021X rear panel: LAN cable in **LAN**; the
grabber's sort TTL coax is in **TRIG IN/OUT** (bottom BNC); **PULSE IN/OUT**
carries only an empty BNC adapter; USB DEVICE unused.

Add one cable — the loopback: SSG **PULSE IN/OUT** → Coaxlink **TTLIO11**.

TTLIO11 and TTLIO12 are the same two lines on every Coaxlink I/O connector,
so the loopback goes on **the same breakout as the existing TTLIO12 sort
wire**. Which pin that is depends on what the breakout is plugged into —
identify it from where the sort wire already sits (Euresys Coaxlink Hardware
Manual 12.5, D205: "Internal I/O 1 Connector" p.40, "External I/O Connector"
p.31, "1625 DB25F I/O Adapter Cable" p.111, "3304 HD26F I/O Adapter Cable";
identical for the PC1633 Quad G3 and PC3603 Quad CXP-12):

| Breakout plugged into | Connector you see | Sort wire TTLIO12 is on | **TTLIO11 (loopback signal)** | **TTLIO11 ground** |
|---|---|---|---|---|
| **1625 DB25F I/O adapter cable** (ribbon from Internal I/O 1 to a DB25 on a bracket) | 25 pins, **2 rows** (13 + 12) | DB25 pin 23 (gnd 11) | **DB25 pin 22** | **DB25 pin 10** |
| Card bracket **External I/O** directly, or the **3304 HD26F adapter cable** from Internal I/O 1 | 26 pins, **3 rows**, smaller shell | HD26 pin 17 (gnd 18) | **HD26 pin 25** | **HD26 pin 24** |
| Bare **Internal I/O 1** header on the card | 26-pin 2-row 0.1" header | header pin 21 (gnd 22) | **header pin 19** | **header pin 20** |

**Rig breakout identified from a photo (2026-10-01): HL-DB26T-mini**, a
26-pin **HD26** terminal board on the Coaxlink **External I/O** connector —
row 2 of the table, not DB25. The existing red/black sort pair sits on
terminals 17/18 (TTLIO12 + GND), which confirms it. So the loopback goes to
**terminal 25 (TTLIO11, BNC centre)** and **terminal 24 (GND, BNC shield)**.
Annotated photos: [`docs/evidence/2026-10-01-trigger-loopback-wiring/`](../../evidence/2026-10-01-trigger-loopback-wiring/README.md).

**Check first:** a green and a yellow wire already land near terminals 23–25
(pen-marked "+"/"−"). If one is on 25, TTLIO11 is already used; identify that
wire before reusing the pin. Fallback input: isolated IIN11 (HD26 pin 3 +,
pin 12 −; `LineInputToolSource = IIN11`, current-sense, TTL-compatible).

Electrical (manual §3.9/3.10, TTL Input/Output): 5 V-compliant 3.3 V LVTTL
receiver, HIGH > 2.0 V, LOW < 0.8 V, **absolute maximum 0 V … 5 V**. Look at
the SSG PULSE OUT on the scope first: it must stay within 0–5 V (no negative
undershoot, nothing above 5 V); a 5 V TTL or 3.3 V source is fine.

On the SSG (front panel or SCPI over LAN):

- MOD > PULSE > **Pulse Out = ON** (`:PULM:OUT:STATe ON`), Pulse Out
  Polarity = Normal (rising edge = RF on). Everything else stays as it is
  (Pulse Trigger = Ext Trig, Source = Int, your delay/width).
- The user manual says in one place that Ext-Trig is received on PULSE
  IN/OUT (§8.4.4.13) and in another on TRIG IN/OUT (rear-panel section).
  The rig runs with the coax in TRIG IN/OUT; **confirm with the scope that
  PULSE OUT emits one TTL pulse per sort trigger** before trusting the
  loopback. If it does not, fall back to a BNC T on the TTLIO12 line →
  TTLIO11 (then `lineEdgeTimestamp` is the grabber-driven edge, and the RF
  burst is that plus `rf_generator_trigger_delay_s`).

## 4. Application config

The bundled default config now carries a **disabled** `rf_generator` block
and the app merges it into the existing user `config.json` on first start of
this build. In MIB Studio's **Config tab**, section `rf_generator`, set:

```json
{"rf_generator":{"enabled":true,"transport":"lan","resource":"<SSG IP>:5025","timeout_ms":1000}}
```

The SSG's IP is under its System > Interface > LAN. With sorting enabled
(`enable_target_group`), the readiness panel's `rf.generator` gate must read
`pass` with the `*IDN?` string, `trigger EXTernal`, delay and width in the
detail. A `fail` names the link error or the mis-armed setting with its menu
path. `enabled: true` with an empty address reports "set it to the SSG's IP
address". No dedicated UI beyond the Config-tab table (§7).

**Loopback status (2026-10-01):** on this rig both TTL lines are taken —
TTLIO12 (terminals 17/18) drives the SSG TRIG IN, TTLIO11 (terminals 25/24,
yellow/green) drives the LED — so the loopback is **parked**; the user chose
to only make MIB Studio aware of the generator for now. When resumed, the
candidates are: isolated input IIN11 (HD26 pins 3 + / 12 −, ≥ 10 µs pulses,
≤ 50 kHz, opto delay), or TTLIO21 on the card's Internal I/O 2 header via a
1625/3304 cable (full-speed TTL, needs the PC opened). §3/§5/§6 below still
describe the TTLIO11 plan and must be re-targeted to the chosen input.

## 5. `EGrabberCamera::setLineEventCallback` — PARKED (after the loopback input is chosen)

Goal: the looped-back PULSE OUT edge on `TTLIO11` arrives in
`TriggerService::onLineEvent` as a `LineEvent` stamped by the grabber.

### 5.1 GenICam configuration (InterfaceModule, after `start()`)

```cpp
grabber_->setString<InterfaceModule>("LineInputToolSelector", "LIN1");
grabber_->setString<InterfaceModule>("LineInputToolSource", "TTLIO11");
grabber_->setString<InterfaceModule>("LineInputToolActivation", "RisingEdge");
grabber_->setString<InterfaceModule>("EventSelector", "LIN1");
grabber_->setInteger<InterfaceModule>("EventNotification", 1);   // or "EventNotification[LIN1]"
```

Names verified against the Coaxlink GenICam reference (EventControl:
`EventSelector` values `LIN1..LIN8`; IOToolbox: `LineInputToolSource` values
include `TTLIO11`, `LineInputToolActivation` `RisingEdge|FallingEdge|AllEdges`).
Optional: `EventNotificationContext1 = LineStatusAll` to get all line levels
in `data.context1` for a sanity check.

### 5.2 Event delivery — the part that is easy to get wrong

`EGrabberCamera` holds `EGrabber<CallbackOnDemand>` and the capture thread
takes frames with `ScopedBuffer` (`pop()`), which does **not** dispatch I/O
toolbox events. With `CallbackOnDemand`, events are only delivered by a
thread calling `processEvent<...>()`. So:

1. `grabber_->enableEvent<IoToolboxData>();` once (NewBufferData stays as it
   is; the capture loop's `getPendingEventCount<NewBufferData>()` /
   `pop()` are unaffected because the type is explicit).
2. A dedicated small thread (`lineEventThread_`) loops
   `grabber_->processEvent<IoToolboxData>(timeoutMs)` while running; use a
   short timeout (e.g. 100 ms) so `stop()` can join it. Do **not** switch the
   grabber to `CallbackSingleThread`: that model would also route
   NewBufferData into callbacks and break the existing pop-based loop.
3. Override `onIoToolboxEvent(const IoToolboxData& data)` in the grabber
   subclass (or a thin `EGrabber<CallbackOnDemand>` subclass the camera owns)
   and forward:

```cpp
void onIoToolboxEvent(const IoToolboxData& data) override {
    if (data.numid != EVENT_DATA_NUMID_IO_TOOLBOX_LIN1) return;  // GenTL_v1_5_EuresysCustom.h
    camera::common::LineEvent ev;
    ev.line = "TTLIO11";
    ev.rising = true;                       // LIN1 is configured RisingEdge
    ev.timestamp = data.timestamp;          // µs since boot, same clock as BUFFER_INFO_TIMESTAMP
    ev.hostTimestampUs = backend::Tools::getTimestamp();
    callback(ev);                           // copy of the std::function taken under its own mutex
}
```

4. `setLineEventCallback(cb)` stores the callback under a dedicated mutex
   (not `stateMutex_`, which `stop()` holds for ~360 ms), returns `true`
   when `MIB_HAS_EGRABBER`, `false` from the stub build. Empty callback =
   unsubscribe (keep the thread; it just drops events).
5. Lifecycle: start the thread in `start()` after the nodes are set; in
   `stop()` clear running, join the thread **before** `grabber_` is reset
   (the thread touches `grabber_`); `TriggerService::setCamera(nullptr)`
   already unsubscribes before the camera is destroyed.
6. The callback runs on the event thread while `TriggerService` may be
   mid-pulse on the trigger thread: `onLineEvent` takes only its leaf
   `eventMutex_`, so no lock-order issue — but do not call back into the
   camera from it.

### 5.3 Where it lands

- `include/backend/camera/egrabber/EGrabberCamera.h`: declare
  `bool setLineEventCallback(camera::common::LineEventCallback) override;`
  plus `lineEventMutex_`, `lineEventCallback_`, `lineEventThread_`,
  `lineEventRunning_`.
- `src/backend/camera/egrabber/EGrabberCamera.cpp`: both the
  `MIB_HAS_EGRABBER` implementation and the stub (`return false`).
- Vault: `knowledge_map/camera/EGrabberCamera.md` (new "Line events" section
  — nodes, the processEvent thread, the clock), one line in
  `knowledge_map/camera/ICamera.md` replacing "follow-up", and the
  acceptance evidence folder (§6) linked from
  `knowledge_map/current-state/Recent-Work.md`. `python3 scripts/check_docs.py`
  before committing (vault maintenance is mandatory, see `AGENTS.md`).

## 6a. Acceptance for THIS hand-off (`docs/evidence/2026-10-xx-ssg-lan-link/`)

1. **Build:** `windows-ninja` Release builds clean, including
   `ScpiTransportVisaWin32.cpp` (never compiled before — fix any MSVC error
   there, keep the runtime `LoadLibrary` design). `ctest` with the backend
   labels passes; list any failure with its output.
2. **Config merge:** first start of the new build adds the `rf_generator`
   section to the user `config.json` (disabled). Confirm it appears in the
   Config tab and that no other user value changed (diff the file before /
   after).
3. **Link:** set `enabled: true`, `resource: "<SSG IP>:5025"` (the user
   provides the IP or it is read from the SSG: System > Interface > LAN).
   With target-group sorting on, the readiness panel shows `rf.generator =
   pass` with the real `*IDN?` string, trigger mode, delay and width. Paste
   that gate line into the evidence README. Also capture one `fail` on
   purpose (wrong IP) and paste its message.
4. **Recorded run:** one short sorting experiment; in the HDF5 file check
   `rf_generator_*` attributes are present and match the SSG front panel,
   `/trigger_events` has one row per sort request with `outcome` 0 and
   ordered `requestUs ≤ wakeUs ≤ fireUs ≤ pulseDoneUs` (`lineEdge*` columns
   are expected to be 0 — no loopback yet), and, if multi-image was on,
   `/valid_frames/series_meta` rows are present and `series_contiguous` is 1.
   Note the median and max of `fireUs − grabUs` (classification frame →
   sort edge driven by the PC).
5. Vault: add the evidence link to `knowledge_map/current-state/Recent-Work.md`
   and tick the on-rig items in
   [`2026-09-30-trigger-frame-alignment.md`](2026-09-30-trigger-frame-alignment.md);
   `python3 scripts/check_docs.py`.

## 6. Acceptance for the loopback — PARKED (`docs/evidence/2026-10-xx-trigger-loopback/`)

1. **Link check:** readiness `rf.generator` = pass with the real `*IDN?`;
   `PULM:OUT:STATe` reads 1. Paste the gate line into the README.
2. **Loopback check (no scope needed):** run a mock-free sorting experiment
   with the camera triggered as usual; in the file, every `/trigger_events`
   row with `outcome == 0` must have `lineEdgeTimestamp != 0`, and
   `TriggerService::getUnpairedLineEdgeCount()` (log it at stop) must be 0.
   Report the distribution of `lineEdgeTimestamp - fireUs` (PC write → edge
   seen by the grabber; expect tens of µs, PCIe + SSG trigger latency) and of
   `lineEdgeTimestamp - grabUs` (classification frame → RF-on).
3. **Frame placement:** for a handful of pulses, show which `series_meta`
   members' `timestampNs` bracket `lineEdgeTimestamp`
   (`+ rf_generator_trigger_delay_s`, `+ pulse_width_s`) — that is the
   exposure the RF burst overlapped. One table in the README.
4. **Scope cross-check (once):** CH1 = TTLIO12 (sort edge), CH2 = SSG PULSE
   OUT; the CH1→CH2 delay should match `rf_generator_trigger_delay_s` plus
   the SSG's internal latency; note the number.
5. Existing guards must stay green on the rig build:
   `backend.trigger_event_log`, `backend.rf_generator_service`,
   `recording.trigger_alignment_roundtrip`, `integration.e2e_series_alignment`,
   `integration.e2e_trigger_timing` (latency budget), `backend.trigger_session`.
   Add a `tests/hardware/hw_trigger_loopback_test.cpp` (label `hardware`,
   `SKIP_RETURN_CODE 77` when no grabber, pattern of
   `hw_illuminated_live_test.cpp`) that binds the live camera, fires
   `TriggerService::manualPulse()` N times and asserts N paired edges.

## 7. Not done — the consumer's list

- `EGrabberCamera::setLineEventCallback` (§5) and its hardware test (§6).
- CIC cycle events (`EventSelector` `CameraTriggerRisingEdge` /
  `StrobeRisingEdge` on the DeviceModule) for exposure-start stamps — same
  mechanism as §5, lower priority.
- Frontend: no UI for the `rf_generator` block; no display of
  `/trigger_events`, `series_meta` or `rf_generator_*` in the Review tab;
  Python reader (`bindings/python`, `scripts/export_hdf5.py`) does not export
  the new datasets yet.
- Setting the SSG sort window from the experiment profile
  (`RfGeneratorService::applySortWindow` exists and is verified; nothing
  calls it).
- Pre-existing: a multi-image series pending at `endExperiment()` is saved
  only when one more frame is processed; if capture stops in the same
  instant the partial series is lost (vault: ProcessingService note).
- MindVision rig: no stamped inputs; needs the camera-tick ↔ host-clock fit
  from the plan before pulses can be placed there.

## 8. Behaviour notes

- The RF generator is read back at **readiness time**, not at stop; the
  stored provenance is what was verified before Start.
- A `rf.generator` fail never writes to the instrument; it reports the
  remedy. `applySortWindow` is the only write path and verifies by readback.
- `TriggerService` pairs edges FIFO; an edge with no pulse (e.g. a manual
  front-panel trigger on the SSG) is counted in
  `getUnpairedLineEdgeCount()`, never attached.
- Timing never goes over LAN/USB; SCPI is ms-scale and jittery.

## 9. Prompt for the next agent

> Branch `feat/trigger-frame-alignment` of `gavinlouuu-kpt/mib-studio-qt`
> (Windows rig PC: Coaxlink + CoaXPress camera, SIGLENT SSG3021X on LAN).
> Read `docs/exec-plans/active/2026-10-01-trigger-frame-alignment-rig-handoff.md`
> first — §0 is the scope — then `AGENTS.md`. Build with the `windows-ninja`
> preset (fix any MSVC error in `ScpiTransportVisaWin32.cpp`) and run the
> backend tests. Start MIB Studio once so the `rf_generator` section is merged
> into the user config, then in the Config tab set `enabled: true` and
> `resource: "<SSG IP>:5025"` (ask the user for the IP, or read it from the
> SSG: System > Interface > LAN). Do the §6a checks: `rf.generator` readiness
> gate passes with the real instrument, one deliberate failure, one short
> sorting run whose HDF5 file has `rf_generator_*`, `/trigger_events` (and
> `series_meta` if multi-image). Put the evidence in `docs/evidence/`, update
> the vault per §6a.5, run `python3 scripts/check_docs.py`, commit on the same
> branch and push. Do **not** add cables, do **not** write to the SSG, and do
> **not** start the loopback or `EGrabberCamera` line-event work (§3/§5/§6 are
> parked until an oscilloscope is attached). Open the PR against `develop`
> only if the user asks.
