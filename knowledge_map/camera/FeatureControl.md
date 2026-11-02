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
| `ModuleWriteTracker.h` | Port decorator: records whether a write (or a selector-scoped describe) reached one module. `EGrabberCamera` watches the Interface module with it. |

## Where the guard lives

`BackendFacade::cameraFeature{Tree,Read,Write}Json` (facade `CaptureFeaturePort`)
→ for a write, `ExperimentCoordinator::withIdleConfiguration` (Start is excluded
for the whole write) → `TriggerService::runWhileTriggerIdle`
([[../services/TriggerService]]: `pulseMutex_`, held by the trigger thread across
a whole pulse) → `CaptureService::withCameraFeatures`
holds `cameraMutex_` (pins `activeCamera_` against release; the session token's
generation is the capture generation that set `activeCamera_`; the capability
revision starts at 1 per generation and is stored back after a write) → the
camera's `FeatureSession::withPort` takes the camera's own port guard
(`EGrabberCamera::triggerMutex_`, which already pins `grabber_` against `stop()`
and serializes the trigger thread's `LineSelector`; `MockCamera::featureMutex_`).
The port is open only while the camera runs. `enumerate()` takes the guard once
per node and re-checks the token pinned by the first step, so a stop never waits
for a whole tree; a session change mid-walk is `stale` with no modules, a refusal
mid-walk is `refused` with no modules.

**Refusals (#806 review, finding 1).** A feature call between a sort pulse's two
`setTriggerOutput` edges would stretch the pulse to an SDK call and sort the wrong
cell. So every feature operation (tree, read, write) is refused, with outcome
`refused`, a reason and no device access, while the sort trigger output is armed
(a camera bound for trigger output) or the trigger service is running, and while
an experiment is Starting, Active or Stopping. Writes are also refused while a
frame recording or realtime processing runs or autofocus is enabled
(`configApplyBlocker` minus capture), and when the coordinator is not idle. Each
check is made under the locks above (`runWhileTriggerIdle` holds `pulseMutex_`
for the whole operation; the run-state gate, `services::CameraFeatureGate`, is
evaluated inside `withCameraFeatures` before the port), so there is no
check-then-act gap. `GuardedPort::run` returns `Access::Refused` for these.

**Consequence (owner decision, stated here so it is not mistaken for a bug):**
in the default boot `AppBackend` binds and starts the trigger service with every
capture session, so feature I/O is refused whenever capture runs, unless the
trigger service is disabled (`MIB_DISABLED_SERVICES=trigger`). Using the node map
on a sorting rig needs an explicit disarm, which is not built.

Device quirk (Coaxlink): `EGrabberCamera::withPort` wraps the port in a
`ModuleWriteTracker` for the Interface module and clears `triggerLineApplied_`
only after a write there or a selector-scoped describe there (anything that could
move `LineSelector`), so the next sort pulse re-selects its line; reads elsewhere
leave it applied.

**Absent modules (finding 4).** A port throws `ModuleAbsent` when its node map has
no such module; the tree reports it `enumerated: false` with reason `Absent: ...`,
never an empty enumerated tree (`FakeNodeMap` does this for any module it does not
define).

**The tree is a job (finding 2).** The bridge runs the walk on the facade's
worker (`startCameraFeatureTree` / `fetchCameraFeatureTree`), so the Rust bridge
lock is held only for the start and each fetch; one walk is in flight (an
identical request joins it, another is refused), see [[../architecture/Rust-Bridge]].

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
retried. Refusals before the device: see *Refusals* above.

## Validation

- `camera.feature_control`, `camera.feature_tree` (fake ports, generated names,
  no production feature name; guard refusals before the port and mid-walk; absent
  modules; `ModuleWriteTracker`), `camera.capture_features` (CaptureService +
  MockCamera + JSON, the run-state gate under the guard, stop/start stress for the
  TSan lane).
- `camera.trigger_feature_interlock`: refused while armed or running; a
  contention stress (pulse thread binding, pulsing and unbinding vs a feature
  thread) shows no feature operation between a pulse's edges on one timeline.
- `e2e.camera_feature_facade`: through `BackendFacade` with the real
  `AppBackend`: trigger armed refuses tree/read/write; experiment
  Starting/Active/Stopping refuse all three; frame recording and autofocus refuse
  writes; the tree job's in-flight-once rule.
- `tools/egrabber_acceptance` builds the SDK-free core (UBSan workflow); with
  `-DEGRABBER_INCLUDE_DIR` the adapter test checks the composed query strings
  and the explicit instantiation compiles against the real headers.
- Real server: `crates/mib-bridge-server/tests/camera_features.rs` (mock node
  map, evidence fixture/mock).
- **No hardware evidence.** The Windows rig run (tree equal to the vendor tool's
  tree for one camera) is E-5.
