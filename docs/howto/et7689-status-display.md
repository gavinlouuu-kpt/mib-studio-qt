# ET7689 YOFO Studio status display

This is a **bounded, read-only bench companion** for the verified PZ7035 ↔
ET7689 UART path. It is a status indicator, not a replacement for YOFO
Studio's controls and not a full touch UI.

## State contract

The companion sends exactly one Studio WebSocket command:
`fetch_instrument_status`. It never sends `take_control`, acquisition,
experiment, mode, LED, ring, or safe-state commands.

| Page | Colour | Meaning |
| ---: | --- | --- |
| 8 | Blue | No usable report: unavailable, malformed, disconnected, transport failure, or stale |
| 6 | Red | A fresh report explicitly reports an error, LED guard fault, core/profile mismatch, receiver-heal give-up, or warning |
| 7 | Green | Fresh valid report received with no reported fault/warning |

Green means **“report received”** only. It does not mean safe, running,
connected for control, or permission to acquire. A missing or stale report is
fail-closed blue.

## Dry-run validation

From the MIB Studio repository root, one bounded sample is printed as JSON and
does not touch the display:

```bash
python3 tools/display_status/yofo_status_display.py \
  --studio-url http://127.0.0.1:8427
```

For a finite observation window:

```bash
python3 tools/display_status/yofo_status_display.py \
  --studio-url http://127.0.0.1:8427 \
  --duration 60 --poll 1
```

The poll and request deadlines are bounded. Transport failures immediately
produce blue and the next bounded poll may reconnect. The process does not
keep an unbounded cache or log.

## Bench display mode

The physical display mode requires the separately reviewed
`pz_display_colors.py` adapter from the PZ7035 display-UART worktree. Pass its
path explicitly; the companion imports its `DisplayLink`, layout inspection,
UART framing, and **same exclusive lock** rather than copying the protocol:

```bash
python3 tools/display_status/yofo_status_display.py \
  --studio-url http://127.0.0.1:8427 \
  --display-tool /path/to/pz7035-display-uart/scripts/pz_display_colors.py \
  --duration 300
```

Before the first page-register write the adapter verifies the known ET7689
layout and that pages 6/7/8 contain the installed solid-colour records with
page-local overlays disabled. No flash write is performed. Startup paints blue;
subsequent writes happen only when the classified page changes and every page
write is read back. The original page and brightness are restored in cleanup,
and the adapter disables UART TX before closing.

Keep the display's normal USB power and the FPGA's UART wiring as already
verified. Do not run the colour installer or another UART client concurrently.

## Integration boundary

The current implementation is a **bench bridge only**: Linux YOFO Studio's
native UART/status integration is still outstanding, and the bare-metal UART
bridge cannot coexist with an independent native Linux UART owner. In
particular, the bench UART register window currently collides with the PZ main
receiver/self-heal MMIO window; it must not be opened by standing Linux Studio
code. Production integration needs a new capability and non-colliding address
map, reviewed with the FPGA owner. Do not advertise this as deployed Studio
integration until that ownership boundary is implemented and accepted.

This records the follow-up decision: Gavin authorized a new status screen, but
the previous defer boundary for native integration remains in force until the
integration is reviewed and accepted. The hardware proof and wiring reference
are tracked in [PR #82](https://github.com/gavinlouuu-kpt/pz7035-imx426/pull/82).

## Checks

Offline tests (no Studio, FPGA, or display required):

```bash
python3 -m unittest -v tests/test_display_status.py
python3 scripts/check_docs.py
```
