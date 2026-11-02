# Typed feature control and the GenApi node map

**Source:** `include/backend/camera/features/`, `src/backend/camera/features/`
(namespace `camera::features`).
**Related:** [[ICamera]], [[EGrabberCamera]], [[MockCamera]], [[../services/CaptureService]],
[[../architecture/Rust-Bridge]] (ABI 49).

Goal E02 (#806): MIB drives the camera **through the camera's own GenApi node
map at runtime**. Features, categories, type, access, visibility, unit, bounds
and enum entries are enumerated from the connected device; no hand-kept list,
JSON or XML of feature names decides what exists or is writable. Access is
re-read on every write (before and after) and a write advances the capability
revision, because selectors and dependent nodes move access.

## Layers

| File | Role |
|---|---|
| `FeatureControl.h/.cpp` | `Port` (describe/write + E-1 enumeration: `categories`, `categoriesOf`, `featuresOf`, `features`, `info` → `NodeInfo` display name / tooltip / description / visibility; `canEnumerate()` false by default so a port without introspection is *unsupported*), `read`/`write` with session fencing, `validateAddress`/`validateValue`. |
| `FeatureTree.h/.cpp` | `GuardedPort` (service-side guard), `enumerate()` (tree walk), `guardedRead`/`guardedWrite` (revision advance), name conversions. SDK-free. |
| `FeatureSession.h` | The optional camera capability: `ICamera::featureSession()` returns one (or nullptr = unsupported). `source()`, `evidence()` ("device" or "fixture/mock"), `withPort(fn)` under the camera's port guard. |
| `EGrabberFeaturePort.h` | Header-only adapter over a borrowed `Euresys::EGrabber`. |
| `FakeNodeMap.h/.cpp` | Data-driven in-memory node map (fixture). Access evaluated per describe (`writableWhen`), selected nodes per selector entry, write re-checks access/type/bounds. Owner decides its contents. |
| `FeatureJson.h/.cpp` | Request parsing and reply rendering for the bridge commands. |

## Where the guard lives

`BackendFacade::cameraFeature{Tree,Read,Write}Json` → `CaptureService::withCameraFeatures`
holds `cameraMutex_` (pins `activeCamera_` against release; the session token's
generation is the capture generation that set `activeCamera_`; the capability
revision starts at 1 per generation and is stored back after a write; the token's
`instance` is `CaptureService::instanceId()`, drawn per backend instance, so a token
from before a backend restart is stale although generations restart at 1, E01) → the
camera's `FeatureSession::withPort` takes the camera's own port guard
(`EGrabberCamera::triggerMutex_`, which already pins `grabber_` against `stop()`
and serializes the trigger thread's `LineSelector`; `MockCamera::featureMutex_`).
The port is open only while the camera runs. `enumerate()` takes the guard once
per node and re-checks the token pinned by the first step, so neither a stop nor
a trigger pulse waits for a whole tree; a session change mid-walk is `stale` with
no modules.

Device quirk (Coaxlink): after any feature access `EGrabberCamera` clears
`triggerLineApplied_`, so the next sort pulse re-selects its `LineSelector`.

## eGrabber introspection queries

Verified against the SDK header `internal/common/GenapiBuilders.h` (namespace
`Euresys::query`; headers from the owner's `vcpkg-EGrabber` repo, compile-only on
Linux): `categories(bool)` → `@categories`, `categoriesOf(cat, bool)`,
`featuresOf(cat, bool)` → `@[!]features <cat>`, `features(bool)`,
`info(feature, what)`, plus #812's `implemented`, `readable`, `writeable`,
`interfaces`, `enumEntries`. `availableOnly=true` maps to the `@!` qualifier.
**Unverified on hardware:** the `info` child names `DisplayName`, `Description`,
`Visibility` (GenICam schema) and the tooltip, tried as `ToolTip` (schema) then
`Tooltip` (the SDK header's example); and whether `featuresOf` lists child
categories (the walk drops category names from feature lists either way). Node
names are validated as bounded identifiers before they are spliced into a query.

## Outcomes

`read`, `readback`, `written_unconfirmed`, `refused`, `stale`, `unsupported`,
`error`, `unknown_write_outcome` stay distinct; `requested` is reported apart
from `readback`. An exception after submission is an unknown outcome, never
retried. Writes are refused while an experiment is starting/active/stopping or a
frame recording runs.

## Validation

- `camera.feature_control`, `camera.feature_tree` (fake ports, generated names,
  no production feature name), `camera.capture_features` (CaptureService +
  MockCamera + JSON, stop/start stress for the TSan lane).
- `tools/egrabber_acceptance` builds the SDK-free core (UBSan workflow); with
  `-DEGRABBER_INCLUDE_DIR` the adapter test checks the composed query strings
  and the explicit instantiation compiles against the real headers.
- Real server: `crates/mib-bridge-server/tests/camera_features.rs` (mock node
  map, evidence fixture/mock).
- **No hardware evidence.** The Windows rig run (tree equal to the vendor tool's
  tree for one camera) is E-5.
