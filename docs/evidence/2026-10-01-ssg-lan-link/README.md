# SSG3021X over LAN on the rig PC — 2026-10-01

Acceptance evidence for hand-off §6a of
[`2026-10-01-trigger-frame-alignment-rig-handoff.md`](../../exec-plans/active/2026-10-01-trigger-frame-alignment-rig-handoff.md),
collected on the rig PC (Windows 11, i9-13900, Coaxlink Quad CXP-12 +
EoSens2.0MCX12, SIGLENT SSG3021X) from branch `feat/trigger-frame-alignment`
at `a5244e54` plus the fixes in this commit. Nothing was wired and nothing
was written to the SSG.

| §6a item | State |
|---|---|
| 1. Build + tests | **Done.** Builds after three fixes (below); fast lane 140/141, integration lane 12/13, both failures outside this branch |
| 2. Config merge | **Done, with a finding:** the `rf_generator` block is *not* merged on this rig (profile config), and the first start changed one unrelated user value |
| 3. Link: `rf.generator` gate | **Fail cases done** in the real app. **Pass is blocked:** the PC has no address on the SSG's subnet (needs an administrator) |
| 4. Recorded sorting run | **Not done** — depends on item 3 |
| 5. Vault | Done for the above; the plan's on-rig box stays unticked |

## 1. Build and tests

`windows-ninja` preset, Release, MSVC 14.41 (VS 2022), `MIB_ENABLE_HARDWARE_SDKS=ON`.

`src/backend/services/ScpiTransportVisaWin32.cpp` compiled on the first
attempt, no error and no warning; it needed no change. The build stopped on
three other things:

| File | Problem | Fix |
|---|---|---|
| `include/backend/processing/MonitoringDensity.h` (from `develop`) | a local named `far`; `<windows.h>` defines `far` as an empty macro, so `AppBackend.cpp` (which includes the EGrabber headers first) failed with C3329/C2760 | renamed to `farEnd` |
| `tests/processing/processing_core_v2_plugin_test.cpp` (from `develop`) | includes `<dlfcn.h>`; registered on every platform | `LoadLibrary`/`GetProcAddress` shim under `_WIN32`; the test passes |
| `tests/integration/e2e_series_alignment_test.cpp` (this branch) | `target_group_area_max = 1e12` into an `int` field: out of range, MSVC produced a negative gate, no frame was a target, `pulses=0`, `/trigger_events` absent | `std::numeric_limits<int>::max()`; now `events=239 fired=239 aligned=239 ordered=239` |

The third one is a test defect, not a product defect: the app's gates come
from the config as integers.

Results after the fixes (logs in this folder):

- [`ctest-windows-ninja-test.log`](ctest-windows-ninja-test.log) — fast
  lane, **140 of 141 pass**. All guards of this work are green:
  `backend.trigger_session`, `backend.trigger_event_log`,
  `backend.rf_generator_service`, `backend.experiment_readiness`,
  `recording.trigger_alignment_roundtrip`, `processing.core_v2_plugin`.
- [`ctest-windows-ninja-integration-test.log`](ctest-windows-ninja-integration-test.log)
  — integration lane, **12 of 13 pass**, including
  `integration.e2e_series_alignment` and `integration.e2e_trigger_timing`.

Failures, both in code this branch does not touch, both failing on every
run on this PC (5 and 3 runs):

- `frontend.mainwindow_shutdown` — `EXPECT FAILED: !timedOut … closing main
  window exits even with another top-level widget`. The test allows 1 s;
  on a PC with a real Coaxlink the close waits ~1.4 s for the camera
  discovery probe to return (`DeviceDiscoveryService: shutdown draining 2
  worker(s)` → `job 1 Cancelled` 1.38 s later).
- `integration.monitoring_kde_e2e` — `REQUIRE FAILED:
  processing.getMonitoringValidAppended() > 0 … cells reach the monitoring
  ring` (synthetic frames, mock camera). Not investigated.

Tracked as TD-20 in the tech-debt tracker.

## 2. Config merge

The app on this rig does not read `include/config.json`: QSettings
`Config/ExternalAppConfigPath` points at the active **profile**,
`%LOCALAPPDATA%\MIB_Studio_Qt\include\profiles\0929_YUHUI\config.json`.
`AppConfigWatcher` merges new default keys only into the app-managed
`include/config.json`, never into an external path, and a profile file
counts as external. Diff of the profile config before / after the first
start of this build (`jsondiff.py`):

```
keys before: 81  after: 83
added (2):
  + autofocus_backend = "auto"
  + autofocus_endpoint = "COM6"
removed (0):
changed (1):
  ~ autofocus_initial_voltage: 26.0 -> 0.0
```

So:

- **`rf_generator` was not added.** The Config tab only lists keys that are
  in the file, so it could not be enabled from there either. The block was
  added to the profile file by hand (`set_rf_generator.py`, same four keys
  as the bundled default); the running app reloaded it and the Config tab
  then showed an `rf_generator` group. Every profile on a rig needs the same
  edit until the merge covers profiles (TD-21).
- **A user value changed:** `autofocus_initial_voltage` went from 26.0 to
  0.0. The nanopositioner tab (code from `develop`, not this branch) saves
  the stage's current voltage on auto-connect, and this build connects
  observe-only, so it wrote the parked 0 V over the profile's 26 V. The
  value is **still 0.0** in `0929_YUHUI`; restore it before that profile is
  used for focusing. The installed build, run on the same profile half an
  hour earlier, had left 26.0 in place.

Profile config now differs from its pre-session state by exactly: the two
`autofocus_*` keys added, `autofocus_initial_voltage` 26.0 → 0.0, and

```json
"rf_generator": {"enabled": true, "resource": "10.11.13.220:5025", "timeout_ms": 1000, "transport": "lan"}
```

## 3. Link

### Why the PC does not reach the SSG

`Test-NetConnection 10.11.13.220 -Port 5025` fails (ping and TCP). The
instrument is connected and alive: an ARP request for 10.11.13.220 sent
out of `Ethernet 3` (HPE 561T port, 100 Mbps link, direct cable) is
answered by `74-5B-C5-23-38-4F`, the MAC on the SSG's front panel. But
`Ethernet 3` only has its link-local address `169.254.59.84/16`, no
interface is in `10.11.13.0/24`, and the default route is a Tailscale exit
node, so packets for the SSG leave through the tunnel.

Remedy, in an elevated PowerShell (this session is not elevated):

```powershell
New-NetIPAddress -InterfaceAlias "Ethernet 3" -IPAddress 10.11.13.10 -PrefixLength 24
Test-NetConnection 10.11.13.220 -Port 5025
```

No gateway is needed; the on-link /24 route wins over the Tailscale default.

### Gate lines captured in the real app

MIB Studio (this build), live Coaxlink camera, Experiment tab, profile with
`target_group.enabled = true`; Start Experiment pressed through UI
Automation (`gate.ps1`). In all three cases `rf.generator` was the **only**
blocking gate, and nothing was sent to the instrument.

`resource: ""` (enabled, no address):

```
Blocked — rf.generator: RF generator link is down: rf_generator.resource is empty: set it to the SSG's IP address (e.g. "192.168.1.50:5025")
    → check the USB/LAN connection and that the instrument is an SSG3000X
```

`resource: "10.11.13.221:5025"` (the deliberate wrong IP):

```
Blocked — rf.generator: RF generator link is down: cannot open RF generator link (lan 10.11.13.221:5025): connect to 10.11.13.221:5025 failed: connect timed out after 2000 ms
    → check the USB/LAN connection and that the instrument is an SSG3000X
```

`resource: "10.11.13.220:5025"` (the right address, PC not on the subnet):

```
Blocked — rf.generator: RF generator link is down: cannot open RF generator link (lan 10.11.13.220:5025): connect to 10.11.13.220:5025 failed: connect timed out after 2000 ms
    → check the USB/LAN connection and that the instrument is an SSG3000X
```

The gate does its job — a sorting run cannot start while the generator is
unverified — but the wrong-IP and no-route messages are identical, and the
remedy text does not mention the PC's own address.

**Not yet shown:** `rf.generator = pass` with the real `*IDN?` string,
trigger mode, delay and width.

## 4. Recorded run — open

No sorting run was recorded. To finish, once `Test-NetConnection` succeeds:

1. Start this build, open the Experiment tab, press Start Experiment. With
   the gate passing the save dialog opens; record a short run with sample
   flowing so that targets occur.
2. `python check_run.py <file.h5>` prints the stored `rf.generator` gate
   line, the `rf_generator_*` attributes, the `/trigger_events` checks
   (`outcome` 0, `requestUs ≤ wakeUs ≤ fireUs ≤ pulseDoneUs`, `lineEdge*`
   0), median and max of `fireUs − grabUs`, and `series_meta` /
   `series_contiguous` when multi-image is on. Paste its output here and
   compare the attributes with the SSG front panel.

`check_run.py` has not been run against a real file yet.

## Files

| File | What |
|---|---|
| `ctest-windows-ninja-test.log`, `ctest-windows-ninja-integration-test.log` | test lanes after the fixes |
| `check_run.py` | §6a.4 checks on a recorded file (read-only) |
| `gate.ps1`, `uia.ps1` | set the address, press Start Experiment, read the readiness dialog |
| `set_rf_generator.py`, `jsondiff.py` | edit the `rf_generator` block; flat JSON diff used for item 2 |
