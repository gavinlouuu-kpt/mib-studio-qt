// Connect stage rules (epic #863, B1): what the device lists show, what the status line says about a scan,
// which row Connect acts on, why a button is blocked, and what the startup selection found. Pure functions:
// the wording is Qt's (src/frontend/tabs/ConnectTab.cpp, src/frontend/system/DeviceInitManager.cpp).
import type {
  CameraDiscovery, CameraSelection, DiscoveredCamera, DiscoveredFramegrabber, DiscoverySnapshot, FrameDeliveryMode,
} from "../../bridge";
import {
  CAMERA_SELECTION_MODES, CAMERA_TYPES, DISCOVERY_DEVICE_KINDS, DISCOVERY_ERROR_KINDS,
  DISCOVERY_IDENTIFICATION_STATUSES, DISCOVERY_JOB_STATES,
} from "../../bridgeContract";

export type DeviceTab = "cameras" | "mindvision" | "framegrabbers";
export type PickedDevices = Record<DeviceTab, string | null>;
export const NO_PICK: PickedDevices = { cameras: null, mindvision: null, framegrabbers: null };

export const SCANNING_TEXT = "Scanning for cameras and framegrabbers...";
export const NO_CAMERAS_TEXT = "No cameras found.";
export const MULTIPLE_CAMERAS_TEXT = "Multiple cameras found; select one and click Connect.";

export const cameraKey = (c: DiscoveredCamera) => `${c.camera_type}:${c.interface_index}:${c.device_index}:${c.camera_index}`;
export const framegrabberKey = (g: DiscoveredFramegrabber) => `fg:${g.interface_index}:${g.device_index}:${g.stream_index}`;

export type DeviceRow =
  | { tab: "cameras" | "mindvision"; key: string; label: string; camera: DiscoveredCamera }
  | { tab: "framegrabbers"; key: string; label: string; framegrabber: DiscoveredFramegrabber };

/** The rows of one sub-tab, in the order the backend listed them. The Cameras list also holds the synthetic mock source. */
export function deviceRows(discovery: CameraDiscovery | null, tab: DeviceTab): DeviceRow[] {
  if (!discovery) return [];
  if (tab === "framegrabbers") {
    return discovery.framegrabbers.map((g) => ({ tab, key: framegrabberKey(g), label: g.label, framegrabber: g }));
  }
  return discovery.cameras
    .filter((c) => (tab === "mindvision" ? c.camera_type === CAMERA_TYPES.MindVision : c.camera_type !== CAMERA_TYPES.MindVision))
    .map((c) => ({ tab, key: cameraKey(c), label: c.label, camera: c }));
}

const isMockRow = (row: DeviceRow) => row.tab !== "framegrabbers" && row.camera.camera_type === CAMERA_TYPES.Mock;

/** After a scan, as Qt (`showDiscoveryResults`): row 0 of every list is selected. A row the operator picked earlier
 *  that the new scan still lists stays picked. The synthetic mock source is not a Qt row, so it is never preselected
 *  (Connect would open the mock dialog when nothing was found). */
export function preselectDevices(discovery: CameraDiscovery | null, previous: PickedDevices = NO_PICK): PickedDevices {
  const next: PickedDevices = { ...NO_PICK };
  for (const tab of ["cameras", "mindvision", "framegrabbers"] as const) {
    const rows = deviceRows(discovery, tab);
    const kept = rows.find((r) => r.key === previous[tab]);
    next[tab] = (kept ?? rows.find((r) => !isMockRow(r)))?.key ?? null;
  }
  return next;
}

/** The row Connect acts on: the one picked in the sub-tab that is shown (not a row hidden on another sub-tab). */
export function pickedRow(discovery: CameraDiscovery | null, tab: DeviceTab, picked: PickedDevices): DeviceRow | null {
  return deviceRows(discovery, tab).find((r) => r.key === picked[tab]) ?? null;
}

const DEVICE_NOUN: Record<DeviceTab, string> = { cameras: "eGrabber camera", mindvision: "MindVision camera", framegrabbers: "framegrabber" };

// ---- the status line ---------------------------------------------------------------------------------------

/** `Found N framegrabber(s), N eGrabber camera(s), N MindVision camera(s)` (Qt counts the physical cameras). */
export function foundLine(discovery: CameraDiscovery | null): string {
  const cameras = discovery?.cameras ?? [];
  return `Found ${(discovery?.framegrabbers ?? []).length} framegrabber(s), ` +
    `${cameras.filter((c) => c.camera_type === CAMERA_TYPES.EGrabber).length} eGrabber camera(s), ` +
    `${cameras.filter((c) => c.camera_type === CAMERA_TYPES.MindVision).length} MindVision camera(s)`;
}

const ERROR_KIND_NAMES: Record<number, string> = Object.fromEntries(
  Object.entries(DISCOVERY_ERROR_KINDS).map(([name, value]) => [value, name]),
);
export const errorKindName = (kind: number) => ERROR_KIND_NAMES[kind] ?? "Unknown";

/** The first coverage gap an operator can act on (a busy SDK, a timeout, ...). A compiled-out SDK is expected on a
 *  bench PC and is not one (as Qt). */
export function firstCoverageGap(discovery: Pick<CameraDiscovery, "errors">): { provider: string; kind: string } | null {
  const e = discovery.errors.find((x) => x.kind !== DISCOVERY_ERROR_KINDS.MissingSdk);
  return e ? { provider: e.provider_id, kind: errorKindName(e.kind) } : null;
}

/** What a finished scan says (Qt `showDiscoveryResults`): a cancelled scan, an incomplete one and a failed one never
 *  read as a plain "0 cameras found". */
export function discoveryStatusText(discovery: CameraDiscovery): string {
  if (discovery.state === DISCOVERY_JOB_STATES.Cancelled) return "Camera scan cancelled.";
  let text = foundLine(discovery);
  const gap = discovery.complete ? null : firstCoverageGap(discovery);
  if (gap) text += ` (incomplete: ${gap.provider} ${gap.kind})`;
  else if (discovery.state === DISCOVERY_JOB_STATES.Failed) text += " (scan failed)";
  return text;
}

/** A scan the backend refused or that could not finish (for example while a capture runs). */
export function discoveryProblemText(reason: string): string {
  return `Camera discovery incomplete: ${reason}`;
}

/** The text after the found-line: which camera the backend holds. */
export function selectionSuffix(selection: CameraSelection | null): string {
  if (!selection?.configured) return " · no camera selected";
  const label = selection.mode === CAMERA_SELECTION_MODES.Mock
    ? `mock (${selection.mock_frame_dir || "no folder"})`
    : selection.label || "camera";
  return ` · selected: ${label}`;
}

const NOT_CAPTURING = " (not capturing)";
/** Qt writes "Connected to <label> (not capturing)" once and the label goes stale when the camera starts (report Q1).
 *  Here the line follows the live state: while the camera runs it says "(capturing)". */
export function liveActionNote(note: string, running: boolean): string {
  return running && note.endsWith(NOT_CAPTURING) ? `${note.slice(0, -NOT_CAPTURING.length)} (capturing)` : note;
}

export interface StatusInput {
  scanning: boolean;
  /** The camera runs: an action note that says "(not capturing)" says "(capturing)" instead. */
  running?: boolean;
  /** The last operator action's result ("Connected to ...", "Delivery mode set to ..."); cleared by the next scan. */
  actionNote: string;
  /** The last scan's result (`discoveryStatusText` or a problem text); empty before the first scan. */
  scanNote: string;
  discovery: CameraDiscovery | null;
  selection: CameraSelection | null;
}

/** The one status line on the Connect stage (Qt's `statusLabel`). */
export function connectStatusLine({ scanning, running = false, actionNote, scanNote, discovery, selection }: StatusInput): string {
  if (scanning) return SCANNING_TEXT;
  if (actionNote) return liveActionNote(actionNote, running);
  return `${scanNote || foundLine(discovery)}${selectionSuffix(selection)}`;
}

// ---- why a button is blocked --------------------------------------------------------------------------------

export interface BlockInput {
  ready: boolean;
  running: boolean;
  expActive: boolean;
  /** A camera-script apply or reset is in flight (Config › Camera script). */
  scriptBusy: boolean;
}

/** Qt refuses Connect and Configure Mock while the camera runs ("Cannot change camera connection while camera is
 *  running"); an experiment holds the camera as well. Undefined when the camera source may change. */
export function cameraSourceBlockReason({ ready, running, expActive, scriptBusy }: BlockInput): string | undefined {
  if (!ready) return "Backend is not initialized";
  if (running) return "Cannot change camera connection while camera is running. Stop the camera first.";
  if (expActive) return "Cannot change camera connection while an experiment is active.";
  if (scriptBusy) return "Camera setup is in progress";
  return undefined;
}

export function connectBlockReason(input: BlockInput, tab: DeviceTab, row: DeviceRow | null): string | undefined {
  return cameraSourceBlockReason(input) ?? (row ? undefined : `Select a ${DEVICE_NOUN[tab]} from the list first`);
}

/** The Refresh button: never while a scan runs (it joins that job instead); a refusal while capturing is the backend's
 *  and reaches the status line, so the button stays enabled then. */
export function refreshBlockReason(ready: boolean, scanning: boolean): string | undefined {
  if (!ready) return "Backend is not initialized";
  if (scanning) return SCANNING_TEXT;
  return undefined;
}

// ---- the startup selection ----------------------------------------------------------------------------------

export type StartupCameraOutcome =
  | { kind: "none"; text: string }
  | { kind: "several"; text: string }
  | { kind: "one" }
  | { kind: "incomplete"; text: string };

/** Mirror of the backend's `isDecidable` (StartupDiscoveryPolicy.cpp): a complete scan, or one whose only gaps are
 *  SDKs that are not there. Anything else is unknown coverage, never "no cameras". */
export function isDecidable(snapshot: DiscoverySnapshot): boolean {
  if (snapshot.state !== DISCOVERY_JOB_STATES.Completed || snapshot.overflow) return false;
  if (snapshot.complete) return true;
  if (snapshot.errors.length === 0) return false;
  return snapshot.errors.every((e) => e.kind === DISCOVERY_ERROR_KINDS.MissingSdk || e.kind === DISCOVERY_ERROR_KINDS.Unsupported);
}

/** Mirror of the backend's `decideCamera`: what the startup scan decided, from the job's snapshot. The backend
 *  selects the one identified physical camera itself; the shell only needs to say why nothing was selected. */
export function startupCameraOutcome(snapshot: DiscoverySnapshot): StartupCameraOutcome {
  if (!isDecidable(snapshot)) {
    const gap = firstCoverageGap({ errors: snapshot.errors });
    const stateName = Object.entries(DISCOVERY_JOB_STATES).find(([, v]) => v === snapshot.state)?.[0] ?? "ended";
    const reason = gap ? `${gap.provider} ${gap.kind}` : `camera discovery ${stateName}${snapshot.complete ? "" : " (incomplete coverage)"}`;
    return { kind: "incomplete", text: discoveryProblemText(reason) };
  }
  const identified = snapshot.candidates.filter((d) =>
    d.kind === DISCOVERY_DEVICE_KINDS.Camera && !d.synthetic && d.identification === DISCOVERY_IDENTIFICATION_STATUSES.Identified).length;
  if (identified === 0) return { kind: "none", text: NO_CAMERAS_TEXT };
  if (identified === 1) return { kind: "one" };
  return { kind: "several", text: MULTIPLE_CAMERAS_TEXT };
}

/** The list row the backend's selection points at (so the auto-selected camera is highlighted and its sub-tab opens). */
export function rowOfSelection(discovery: CameraDiscovery | null, selection: CameraSelection | null): DeviceRow | null {
  if (!discovery || !selection?.configured) return null;
  for (const tab of ["cameras", "mindvision"] as const) {
    for (const row of deviceRows(discovery, tab)) {
      if (row.tab === "framegrabbers") continue;
      const c = row.camera;
      if (selection.mode === CAMERA_SELECTION_MODES.MindVision && c.camera_type === CAMERA_TYPES.MindVision && c.camera_index === selection.mindvision_index) return row;
      if (selection.mode === CAMERA_SELECTION_MODES.Hardware && c.camera_type === CAMERA_TYPES.EGrabber
        && c.interface_index === selection.interface_index && c.device_index === selection.device_index) return row;
      if (selection.mode === CAMERA_SELECTION_MODES.Mock && c.camera_type === CAMERA_TYPES.Mock) return row;
    }
  }
  return null;
}

/** A React/DOM click event: the Refresh button hands its event to the scan it starts. */
export const isClickEvent = (x: unknown): boolean => typeof x === "object" && x !== null && "nativeEvent" in x;

export const connectedNote = (label: string) => `Connected to ${label} (not capturing)`;
export const MOCK_CONFIGURED_NOTE = "Mock camera configured (not capturing)";
export const VANISHED_NOTE = "The selected device is no longer listed. Scanning again; pick a device and click Connect.";

// ---- delivery mode ---------------------------------------------------------------------------------------------

export const DELIVERY_MODES: ReadonlyArray<{ value: FrameDeliveryMode; label: string }> = [
  { value: "everyFrame", label: "Every Frame (preserves sequence)" },
  { value: "latestFrame", label: "Latest Frame (lowest latency, may drop)" },
];
