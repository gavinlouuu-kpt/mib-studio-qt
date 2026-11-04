# Dot Grid portal — MIBQT-28

Invite-only CAD preparation service for MIB Studio. This is a reviewable v1 implementation,
not a deployed service or a fabrication qualification.

## What it does

- Verifies Cloudflare Access RS256 JWT signature, issuer, audience and expiry on **every** request,
  then checks current D1 membership. No production authentication bypass exists.
- Tenant administrators grant/revoke member access. Invitations are email allowlist entries;
  this app does not send email. A member belongs to one tenant in v1. Tenant members share designs.
- Accepts DXF/DWG, keeps the original in private R2, requires explicit units, normalizes to
  micrometres with a recorded source-to-mask transform and shows the converted drawing.
- Rejects unsupported or non-planar geometry. Blocks, text, splines, polyline bulges/widths
  must be converted/exploded in the authoring application, not silently ignored.
- Requires user confirmation before queued generation. Preserves device geometry in the
  combined DXF; exports **dots-only** GDS and a ZIP containing registry, codebook, manifest and instructions.
- Uses existing frozen codec contract 1. No core/C++ ABI changes. Seeds are allocated by
  the authoritative D1 database, not per customer; failed reservations are never deleted/reused.
- Registers only after cross-design synthetic checks and all three R2 writes succeed.
  Download authorization rechecks tenant ownership and ready status. Partial outputs are not downloadable.
- Includes an in-app Driver.js tour, skip/close/restart, state-gated steps and saved feedback.

## Architecture

```mermaid
flowchart LR
  A[Invited member / Access login] --> B[Worker: verify JWT and membership]
  B --> C[Private R2 original CAD]
  B --> D[D1 design identity and status]
  D --> E[Queue: one consumer]
  E --> F[Private Python container: convert and inspect]
  F --> G[Preview + unit confirmation]
  G --> E
  E --> H[Existing codec: generate + cross-check]
  H --> I[R2 combined DXF / dot-layer GDS / MIB map]
  I --> J[D1 ready + immutable registry entry]
  J --> K[Authorized download → MIB Studio local registry]
```

The container has no internet access, runs as a non-root user and has ephemeral job files.
A retry rehydrates the source/normalized DXF from R2. D1 and R2, not the container disk,
are the persistent record. The queue's `max_concurrency: 1` is an intentional v1
registration invariant: do not raise it without a registration coordinator/version fence.

## Local verification

From this directory (Node compatible with the pinned Wrangler; Python 3.14):

```sh
npm ci
python3 -m venv .venv
.venv/bin/pip install -r generator/requirements.lock.txt
npm run types
npm run check
npm test
.venv/bin/python -m pytest tests/test_pipeline.py -q
npm run build
npx playwright install chromium --only-shell
npx playwright test
npx wrangler deploy --dry-run --containers-rollout none
```

`npm test` includes real SQLite authorization queries, real RS256 signatures, local
workerd D1/R2 upload, and a real FastAPI/Python job round trip. The latter substitutes
local storage/transport adapters for cloud bindings. The browser test drives the actual
UI/Driver.js with fixture API responses: it is **not** cloud authentication or CAD proof.
No test calls a live Cloudflare account.

Container build from repository root:

```sh
docker build -f tools/dot_grid_portal/Dockerfile -t mib-dot-grid-portal:local .
```

The image compiles hash-pinned GNU LibreDWG 0.14. Its GPL license is retained in the image;
review distribution obligations before redistributing the image. The source release URL
and hash are in the Dockerfile. Conversion failures fail closed; support is not a promise
that every DWG version/entity will convert losslessly. The converted preview remains required.

## Deployment preparation (not performed automatically)

1. Select the authorized Cloudflare account and portal hostname. Containers require account
   eligibility/billing; no resources or spending are provisioned by the local tests.
2. Create a private R2 bucket, D1 database, primary queue and dead-letter queue. Replace the
   placeholder D1 ID, names and Access settings in `wrangler.jsonc`. Keep R2 public access off.
3. Configure a self-hosted Cloudflare Access application for **all paths** on the hostname,
   with a verified identity provider/one-time PIN policy. Set its issuer and audience.
   Do not rely on Access alone: D1 membership remains the customer authorization boundary.
   Keep `workers_dev` and preview URLs disabled and do not enable asset bypass routes.
4. Apply the D1 migration. Inventory **every existing production local registry**, not just
   the bundled registry. Reserve/import its identities before accepting any uploads.
   Produce reviewable bootstrap SQL:

   ```sh
   .venv/bin/python generator/bootstrap.py --tenant-id example --tenant-name 'Example lab' \
     --admin-email admin@example.org --registry /path/to/existing-registry.json > /tmp/bootstrap.sql
   ```

   This does not execute SQL. Review it and apply it to the intended D1 database using Wrangler.
   Omit `--registry` only when there are genuinely no additional production registries.
   Bootstrap fails on duplicate/mismatched identities instead of rewriting them.
5. Build assets and the container, dry-run, then deploy only to the authorized target.
6. Prove two real invited tenants: login, isolation, DXF and representative DWG upload,
   confirmation, generation, download, revoked access, and MIB Studio registry load.
   Confirm queue retries/DLQ handling and current container limits in the target account.

There is deliberately no preset hostname, account ID or live admin email. Obtain any
required deployment credentials through the existing Vault mechanism; never commit them.

## Current bounds and gaps

- 10 MiB upload, 20 designs/tenant/day, 20,000 model-space entities, 20,000 preview vertices,
  512 kB preview metadata, 1,000,000 lattice nodes, 5-minute subprocess timeout, 3 GiB address-space cap.
  These are conservative v1 bounds, **not full-wafer capacity proof**. A 100 mm wafer at 30 µm
  pitch exceeds the current node bound; full-array chunking is outstanding.
- Fixed validated starting geometry: 30 µm pitch, 12 µm dot, 5 µm displacement;
  no mask shrink/expansion compensation is applied (design scale 1.0).
- Existing keep-out geometry is sampled/heuristic. Do not call it manufacturing DRC.
- Some allocated patterns fail the existing synthetic decoder check. They are not registered;
  retry does not weaken that check. A new design upload allocates a new seed. Automated
  candidate-seed search is a future improvement and must retain all burned reservations.
- Registration means a compatible localization map, **not** a new CAD-overlay viewer,
  fabrication approval, or real-camera localization validation.
- D1/R2 are not a distributed transaction. Partial object writes are hidden until the D1 ready
  transition; retry overwrites only the same job's private outputs. Orphan cleanup is not scheduled.
- The job source is trusted internal queue data. Public requests cannot provide tenant IDs,
  seed values, registry entries, object keys or container routes.
- If retries exhaust, the design becomes failed. A DB outage can instead send a message to
  the DLQ; operator recovery must reconcile the row and replay the same identity, not delete seeds.
- Cloud policy, real customer accounts and full native MIB import remain deployment acceptance work.

## MIB Studio handoff

Extract `mib-map.zip`. Its `registry.json` loads through the existing
`dot_grid.registry_path` setting, merging with the bundled registry. If a local registry
already exists, merge entries with ID/seed validation rather than replace that file.
The decoder reports **mask** coordinates; `manifest.json` records the translation/scaling
from the uploaded drawing. The map ZIP contains no other customer's registry entries.

## Superseding customer-delivery / no-dot contract (2026-11-04)

Earlier map-download and sampled-only descriptions above describe the original implementation.
Customer endpoints now serve only combined DXF and dots GDS. Map ZIP remains internal for
integration; no customer role can download it. Seeds/registry records are not customer data.
Old ready outputs without `inspection.no_dot_contract = 1` are blocked pending reinspection.
New inputs require explicit closed `NO_DOT` polylines or circles. Filled interiors plus
whole-dot radius and 50 µm edge clearance are excluded; declared coverage must be confirmed
in the shaded preview. Missing/open/self-intersecting regions fail closed. Region semantics
are explicit, not inferred from layer-0 wall lines. See the current report for the actual
Wafer_soRT conversion/REGION blocker and the corrected illustrative sample.

### Geometry-preserving import and in-app no-dot review (2026-11-04)

The review import accepts ordinary unlabelled closed geometry; a pre-authored
`NO_DOT` layer is optional. When absent, closed LINE/ARC/polyline topology and
circles become **unselected candidates**, not inferred fluid domains. The user
selects interiors in the zoomable preview and confirms coverage. Open fragments
are counted, not healed automatically. Candidate holes are conservatively filled.
Selection changes clear confirmation; the API validates IDs against server-owned
geometry and refuses empty selections. Review-only polygons are never added to
the delivered device DXF. Existing explicit `NO_DOT` geometry remains supported.

`generator/cad_import.py` supports an operator-provisioned converter executable via
`CAD_ODA_EXECUTABLE`. It receives fixed arguments to convert an isolated directory
containing only `device.dwg` to ACAD2000 DXF, without audit/automatic repair. A Linux
headless wrapper can use `xvfb-run -a /path/to/ODAFileConverter "$@"`. Never accept
converter commands or executable paths from upload metadata. Provision and license
the external converter for the intended runtime separately; it is not downloaded
or redistributed by the repository's default Dockerfile. Without it, LibreDWG is
still used, but empty ACIS bodies are explicitly rejected as geometry loss.

The tailnet review uses ODA File Converter 27.10 in a private, non-root image with
offline tests with networking disabled and loopback-only live ingress. All 88 REGION bodies in the private Wafer_soRT DWG
were recovered as planar straight-edge polygons. Curved or holed ACIS regions,
non-XY bodies, missing payloads and disconnected boundaries still fail closed;
this is not general ACIS-kernel support. TEXT/MTEXT are preserved but excluded from
device bounds. Units remain explicit; conflicting header units produce a warning.

Full-wafer **inspection** is now independent of the one-million-node generation
limit. Large inspection geometry is in tenant-private R2, with only its marker
and validated selection IDs in D1. The real wafer imports as a 110000 × 110000 µm
layout (outer circle; inner wafer circle has 50000 µm radius), with 3210 candidate
areas and 20 unresolved open fragments. **Full-wafer dot generation remains blocked**;
this change does not raise generation resource limits or qualify the wafer mask.

Before: DWG → incomplete converter output → generic unsupported-entity error.
After: isolated conversion → payload/topology checks → preserved device preview →
explicit no-dot choices → capacity/coverage gate → existing verified generator.
The in-app “Guide me through no-dot areas” tour demonstrates the review controls,
with restart/skip, selection-gated progress and a feedback checkpoint.

Private actual-source regression (never commit the CAD):

```sh
CAD_ODA_EXECUTABLE=/path/to/headless-converter \
CAD_WAFER_FIXTURE=/private/path/wafer.dwg \
.venv/bin/python -m pytest tests/test_pipeline.py -q
```

Synthetic fixtures cover empty ACIS payloads, planar/non-planar REGIONs, preserved
annotations, explicit selection, geometry-only output, and an independent channel
clearance calculation on exported dots. Import success does not validate the
choice of fluid domains, fabrication dimensions or manufacturing readiness.
