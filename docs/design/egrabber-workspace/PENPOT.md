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
