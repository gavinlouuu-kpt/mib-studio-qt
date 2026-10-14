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
- The previous native Linux UART ownership defer boundary remains until a
  separately reviewed integration is accepted. The present bench UART window
  collides with the PZ main receiver/self-heal MMIO window; production needs a
  new capability and non-colliding address map.

## Validation

`python3 -m unittest -v tests/test_display_status.py` covers classifier edge
cases, the command allowlist, suppressed repaint, and cleanup. The repository
documentation check is run before delivery.
