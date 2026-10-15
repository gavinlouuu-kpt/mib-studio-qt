# Penpot sources and developer handoff

Status: reference handoff; native prototype routing is not complete.

Use this document together with the [implementation brief](README.md),
[reviewed interaction prototype](prototype.html), and
[execution plan](../../exec-plans/active/2026-10-14-egrabber-workspace.md).
The machine-readable IDs below live in [reference.json](reference.json), under
`penpot`; do not depend on this chat or a local download to find the design.

## Canonical design

| Reference | Value |
|---|---|
| Preferred connector | **Penpot Agents P5** (browserless; explicit file IDs) |
| Project / copy | **Agent Sandbox**, MIB/eGrabber sandbox copy |
| Implementation source file | `202e7953-c6e1-8109-8008-c8bf58f9647a` |
| Proposed page | `a06b0eae-6a8a-8053-8008-c6fbecadf017` |
| Page name | **PROPOSED · eGrabber controls · 2026-10-12** |
| Baseline | `revn=124`, `vern=0`; page metadata checked 2026-10-14 |
| Reference scope | 37 proposed views, 00–36; 5,830 shapes reported on the proposed page |
| Original file — reference only, do not edit | `f19036c0-5b90-8025-8008-c3344b28781b` |

The date in the page name is its creation label, not the latest review date.
Re-read the revision before implementing or editing; these IDs identify the
source, but they do not pin a future read to revision 124. The metadata check did
not repeat the earlier full visual/route audit.

## Open it as a developer

This repository is public; the Penpot service is private. Ask the project owner
for the current authorized origin and account/network access through the normal
team channel. Repository access and these UUIDs do not grant Penpot access. Do not
publish the private hostname, credentials, share tokens or recovery-operation IDs.

With `PENPOT_ORIGIN` set locally to the authorized HTTPS origin, print the viewer
link from the repository root. This makes no network request and changes nothing:

```bash
python3 - <<'PY'
import json
import os
from pathlib import Path
from urllib.parse import urlencode, urlsplit

ref = json.loads(Path('docs/design/egrabber-workspace/reference.json').read_text(encoding='utf-8'))['penpot']
origin = os.environ['PENPOT_ORIGIN'].rstrip('/')
parsed = urlsplit(origin)
if parsed.scheme != 'https' or not parsed.hostname or parsed.username or parsed.password or parsed.path or parsed.query or parsed.fragment:
    raise SystemExit('PENPOT_ORIGIN must be an authorized HTTPS origin, without a path, credentials or token.')
query = urlencode({'file-id': ref['file_id'], 'page-id': ref['page_id'], 'section': 'interactions', 'zoom': 'fit'})
print(origin + '/#/view?' + query)
PY
```

In the viewer, select **MIB · tutorial coverage map** for the full workflow, or
**Scatter · inspect a recorded point** for the original scatter interaction flow.
Select the numbered board by its name in the screen list to review later boards.
Board **30 · Reverse engineering · Evidence + delta map** is the overview of the
seven refinement boards, but no completed native flow starting there is claimed.

A board's numeric name prefix is **not** a durable viewer `index`. Do not copy an
old `index=30` URL and assume it still identifies board 30 after reordering. Use
file/page/board UUIDs for agent lookups, and verify the visible board name.

## Open it as an agent

Read first; no lease is needed for inspection. Always supply the sandbox file ID,
not whichever editor happens to be open. The connector uses native read names:

```json
{
  "tool": "penpot_inspect_file",
  "arguments": {
    "file_id": "202e7953-c6e1-8109-8008-c8bf58f9647a",
    "view": "pages",
    "limit": 10
  }
}
```

For bounded shape inspection, use the returned `revn` and `vern` with
`view="shapes"`, this page's ID, and follow returned cursors unchanged. Those
projections omit native prototype and font fields; they cannot verify links.
To inspect one known native object, use the allowlisted read:

```json
{
  "tool": "penpot_read",
  "arguments": {
    "command": "get-page",
    "params": {
      "file-id": "202e7953-c6e1-8109-8008-c8bf58f9647a",
      "page-id": "a06b0eae-6a8a-8053-8008-c6fbecadf017",
      "object-id": "27d9d564-070f-5fcc-93f2-ccfc7e001740"
    }
  }
}
```

That object is the Source opening button on board 30. `get-page` also accepts a
small list of known object IDs. Prefer small native reads over the entire large
page. A failed read does not mean that the object or its content is absent.
For geometry/appearance, use `penpot_render` with the file, page and board UUID;
independently compare the normal viewer when rendering reports a font failure or
omits text. A valid PNG alone is not visual acceptance.

## Screen-to-implementation map

| Work area | Reference views | First place to inspect |
|---|---|---|
| Full workspace / shell (P1) | 00–04, 23, 30 | Coverage map, Align, Live experiment, preparation overview |
| Instrument preparation (P2) | 06, 23–29, 31 | Source recovery; LED driver; coarse motor and fine piezo; storage; software; ready/blocked states |
| Direct controls / alignment (P3) | 01, 05, 09–13, 33 | Image tools, GenApi, selectors, utilities, scripts, multi-bank |
| Acquisition / retention / persistence (P4) | 02–03, 07–08, 14–16, 32, 34–35 | Independent stream/preview/writer status, buffer ownership, finalization |
| Linked scientific review (P5) | 04, 19–22, 36 | Recorded-point inspector, alternate selection, live pin, unavailable image |
| Maintenance and final parity (P6) | 17–18, 27, all 00–36 | Firmware, preferences/help and compatibility; full-workspace regression |

**Fold-in views (experiment revamp).** Seven further proposed views live in a separate
sandbox file, `2c3c14b3-8959-40f7-a29c-2e6202e341b6` (page `202e7953-c6e1-8109-8008-ca01abeab5bf`), drawn in the same visual
language after the 2026-10-14 decisions; `reference.json` carries them under
`penpot.fold_in`. They extend, and never replace, views 00–36:

| View | Board UUID | Slice |
|---|---|---|
| 00 · Fold-in map | `8b742b76-d59d-4cf7-ae47-de4f40f3d46f` | S1 |
| 01 · Prepare: readiness, run plan, calibration record | `14a7d154-9465-4525-b781-ec51ed7edc3d` | S3, S4 |
| 03 · Experiment · Plan | `d6bbb202-e10c-4a77-ad10-b49b8ded7c0b` | S5 |
| 03 · Experiment · Live (queue, stepper, pinned cell) | `368c693c-68ea-42be-92a0-2553f704b158` | S5 |
| 04 · Review · Run overview (per-sample statistics; chapter marks deferred) | `2b5022ad-4791-4710-a352-f3bb4cbb8545` | S6 |
| 04 · Review · Inspector + population criteria | `be010d21-d507-4422-8f1a-61b0e48b6ecc` | reference only (criteria → #523 FJ04) |
| 04 · Review · Compare | `5cd0129c-264a-4861-bd23-21b7cff7b280` | reference only (→ #314 / #523 FJ09) |

**Operation app, Review app and UI language (2026-10-15).** The same sandbox file
carries three more pages, drawn with the same kit, that split the fold-in into the
two shipped products and document the shared UI language (`README.md`, "One UI
language, two products"). Boards are registered in `reference.json` under
`penpot.fold_in.pages`.

| Page / board | Board UUID | Slice |
|---|---|---|
| **Operation app** · page `abaacf41-dfaf-500a-bc29-e6aa946d3273` | | |
| O0 · Operation app map (records / never does) | `7ad763ec-7b0a-408f-bef9-ef9f3d25dcf1` | S1 |
| O1 · Prepare · readiness, run plan, calibration record | `e4c8fe07-f21c-45cb-9f8d-0045e350da41` | S3, S4 |
| O2 · Align · quick controls, focus/LED, overrides | `7832aa3b-faed-4e2c-ae5c-0b72f1e4fecb` | S2 |
| O3 · Experiment · Plan (sample sheet → queue) | `14db68fb-c9d2-4e21-ae5a-bd4291fc6273` | S5 |
| O4 · Experiment · Live (detected-cell strip, click-to-pin) | `22f62c97-3b47-43f4-bb9c-96eb2be8c257` | S5 |
| O5 · Experiment · Run summary (counts, provenance, next) | `eb7b196d-6e3e-47c2-bb65-183b19329704` | S5 |
| O6 · Experiment · Quick look (experiment board, overlay, decisions) | `d8579b82-5140-4ee0-8205-d9f19862d237` | S5 (descriptive only) |
| **Review app** · page `6dcbad3e-bdc3-5c94-9a59-1307866b24c8` | | |
| R0 · Review app map (how it works / never does) | `643f5337-5992-46b6-9854-4b5ccf638c76` | reference (#512) |
| R1 · Open · experiment library | `9c08bf87-acdb-4e3e-aa68-c84fe430a1ae` | reference (#512) |
| R2 · Overview · per-run counts, small multiples | `f1003b66-4b1c-48f7-96eb-ed08b68e3ec4` | S6 |
| R3 · Inspect · record, image, stored vs reprocessed | `dcdbaa6b-9790-4e69-bea4-936747f588b1` | S6 |
| R4 · Populations · analysis plan, statistics, worker | `a65dfe20-fb6c-4629-96e2-2c098998c094` | reference (#523 FJ04) |
| R5 · Compare | `3c1618f7-f60c-445e-abcd-ce2354c247fa` | reference (#314 / #523 FJ09) |
| R6 · Export · metrics, FCS, statistics, options | `1f19ee2f-b36b-481a-afad-b110567072cf` | reference (#523) |
| **UI language** · page `1e1b66e2-283d-5c13-8083-7bc37a320cfd` | | |
| U0 · Chips, vocabulary, colours, chart, record identity, card anatomy | `80ae0c94-466d-4f46-babd-b131d5fef13b` | all slices |

The `penpot.entry_points` registry contains selected UUID anchors, not an export
of every board or hotspot. All 37 views remain in the proposed page; use the map
above and the named screen list for the rest. The other six pages are reference
and design-system context; their exact IDs are also in `reference.json`.

**Begin with these anchors:**

| Purpose | View | Board UUID |
|---|---|---|
| Full tutorial coverage | 00 | `a06b0eae-6a8a-8053-8008-c735ad438ecb` |
| Image-centred alignment | 01 | `a06b0eae-6a8a-8053-8008-c6fc54d1a2f9` |
| Live experiment | 02 | `a06b0eae-6a8a-8053-8008-c6fcfddfd62e` |
| Prepare overview | 23 | `a06b0eae-6a8a-8053-8008-c863a9feae12` |
| Saved scatter-point inspector | 19 | `a06b0eae-6a8a-8053-8008-c7625377f989` |
| Reverse-engineering refinements | 30 | `30ff79e3-1f85-4700-a516-14504c1946c7` |

## Which source wins

Use Penpot for the accepted workspace structure, layout, wording and navigation
intent; use `prototype.html` for the reviewed-v2 asynchronous/state corrections.
Apply both within the real backend's supported capabilities and safety contract.
If a native board still shows an earlier behavior, record the discrepancy rather
than copying it silently. In particular, retain disabled forbidden writes,
confirmed readback, draft-versus-applied buffers, queue drain before close,
per-record evidence ownership and stale-response rejection.

The seven HTML views correspond to Penpot 30–36; they are **not** the full
production information architecture. Preserve Prepare / Align / Experiment /
Review, including LED, focus, storage and software preparation. Illustrative
measurements, E curves, device names and versions must not become live defaults.

## Known gaps and safe continuation

At the 2026-10-14 review, the 25 intended routes / 50 clickable surfaces on boards
30–36 existed but had no installed native navigation. The reviewed local route
plan corrected the board-33 Delta map label ID to
`079279bc-0757-5027-a986-eaa7029a90e8`; do not reuse the earlier mistyped ID.
Some authored labels specified Inter as the family while retaining a Source Sans
Pro font ID. These are known native follow-ups, not fixes made by this branch.

The companion still reports an unresolved write outcome. Before any native edit,
inspect its current activity, have the existing operation reconciled through the
supported maintainer process, confirm no conflicting writers, acquire the exact
scope lease and re-read the objects. Do not clear journals, duplicate unresolved
writes, force revisions, or edit the original file as a workaround. Native
navigation is a separate acceptance gate; it does not prevent implementation
against this versioned design brief and reviewed HTML reference.

For each implementation slice, record the source file/page/board IDs, actual
revision read, any approved deviations and tests in its PR and vault update.
After a permitted native change, verify saved interactions and the normal viewer,
record the new revision and release the lease. Do not describe HTML/mocked tests
as Penpot-native, application integration or hardware tests.
