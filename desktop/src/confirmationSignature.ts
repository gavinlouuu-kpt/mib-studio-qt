// The identity an operator's Preflight confirmation is bound to (UX-1 #305, extended by #713).
//
// A confirmation is stored as the signature it was given for; the workflow compares it with the
// current signature, so any change here invalidates it. Before #713 the signature was only the
// camera (mode, label) and the host core version. It now also binds the PZ7035 PL build and
// weights, the pump and stage identities, the applied profile and the recording destination
// the backend reports.
//
// Rules:
//   - No camera configured: "" (nothing can be confirmed).
//   - A value that has not been read is written as `null`, distinct from a device that was read
//     and is absent (""), so a confirmation given before a device was read does not survive
//     the first reading of it.
//   - Only identities, never readings that move on their own (flow rate, position, run state),
//     so polling does not invalidate a confirmation.
//   - JSON, so no value can be confused with a separator.
//
// Pure module: unit-testable in plain Node.

export interface SignatureDevice {
  /** False when the device was read and is not connected. */
  connected: boolean;
  /** Stable identity: model, serial, port or endpoint. */
  identity: string;
}

export interface SignatureFacts {
  camera: { configured: boolean; mode: number; label: string };
  /** Host processing core: the active version, "" when invalid, null when not read. */
  coreVersion: string | null;
  /** PZ7035 PL core build and weights ids; null when not read or not on this instrument. */
  plCore: { buildId: string; weightsId: string } | null;
  samplePump: SignatureDevice | null;
  sheathPump: SignatureDevice | null;
  /** Coarse Z stage (#464). */
  zStage: SignatureDevice | null;
  /** Fine stage: the nanopositioner / autofocus controller. */
  nanopositioner: SignatureDevice | null;
  /** Applied profile or method id plus its revision; null when none is applied. */
  profile: { id: string; revision: string } | null;
  /** Recording destination the backend reports (PZ7035); null when nothing reports it. */
  storagePath: string | null;
}

const device = (d: SignatureDevice | null) => (d === null ? null : d.connected ? d.identity || "connected" : "");

/** The signature a Preflight confirmation is stored against; "" with no camera configured. */
export function preflightSignature(f: SignatureFacts): string {
  if (!f.camera.configured) return "";
  return JSON.stringify({
    camera: [f.camera.mode, f.camera.label],
    core: f.coreVersion,
    pl: f.plCore ? [f.plCore.buildId, f.plCore.weightsId] : null,
    pumps: [device(f.samplePump), device(f.sheathPump)],
    stages: [device(f.zStage), device(f.nanopositioner)],
    profile: f.profile ? [f.profile.id, f.profile.revision] : null,
    storage: f.storagePath,
  });
}

/** Camera identity alone (Alignment confirmations and the device part of Preflight). */
export function deviceIdentity(camera: SignatureFacts["camera"]): string {
  return camera.configured ? `${camera.mode}|${camera.label}` : "";
}
