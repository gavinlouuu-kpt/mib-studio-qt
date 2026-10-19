# 2026-10-14 — YOFO status display companion

Status: active

## Scope

Provide a bounded, read-only status companion for the already verified
ET7689/PZ7035 bench link. The companion is intentionally not a full touch UI
and does not claim to replace native YOFO Studio integration.

## Decisions

- The only Studio command is `fetch_instrument_status`.
- Unavailable, malformed, disconnected, stale, and transport-failure reports
  are blue (page 8); explicit faults and warnings are red (page 6); a fresh
  valid report without a reported fault is green (page 7).
- The existing reviewed display adapter is loaded by explicit path. Layout and
  installed-colour records are inspected before page-register writes; flash is
  never written by this companion.
- Production integration remains technically incomplete. The present bench UART window
  collides with the PZ main receiver/self-heal MMIO window; production needs a
  new capability and non-colliding address map.

## Validation

`python3 -m unittest -v tests/test_display_status.py` covers classifier edge
cases, the command allowlist, suppressed repaint, and cleanup. The repository
documentation check is run before delivery.

## Verified bench proof — 2026-10-14

All 11 offline contract tests and `scripts/check_docs.py` passed. Independent
review found no remaining material issues after stale-age and malformed-identity
fixes. A local WebSocket fixture exercised the actual companion and FPGA/display
path for 16 seconds: fresh report → green, simulated guard fault → red,
unavailable → blue. Each page write was read back and all three colours were
visually confirmed with webcam snapshots. This was **fixture status, not a live
Studio deployment**. Cleanup completed successfully and the prior RGB demo was
restarted. No MCU or flash contents changed during this test.

Local evidence: `/mnt/hdd/shared/projects/et7689-audit-20261013/studio-fixture-20261014/`
and the adjacent `studio-fixture-20261014.log`. The existing local Studio route
at port 8427 was not listening. Production Linux transport and a labelled/touch
UI remain outstanding.
