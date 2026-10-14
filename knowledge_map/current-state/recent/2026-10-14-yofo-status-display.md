## 2026-10-14 — Read-only YOFO status display companion

Added [`docs/howto/et7689-status-display.md`](../../../docs/howto/et7689-status-display.md)
and `tools/display_status/yofo_status_display.py`, a bounded status reader for
the verified ET7689 bench bridge. It allowlists only
`fetch_instrument_status`, maps blue/red/green to fail-closed unavailable,
reported-fault, and fresh-report states, and optionally repaints the reviewed
ET7689 colour pages without flash writes. The display adapter is explicit and
the original page, brightness, lock, and UART-TX cleanup are preserved.

This is bench-only. The current bench UART register window collides with the
PZ main receiver/self-heal MMIO window, so native Linux YOFO Studio ownership
remains deferred until a new capability and non-colliding address map are
reviewed and accepted; green is not a safety or permission indication.
