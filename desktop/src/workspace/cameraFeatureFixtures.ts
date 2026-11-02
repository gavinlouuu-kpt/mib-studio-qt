// Test fixtures only (#806 E-3): camera_feature_tree / camera_feature_write replies in the
// contract's shape (bridge-contract.json camera_features), mirroring the mock camera's fake node
// map (src/backend/camera/mock/MockCamera.cpp, evidence "fixture/mock"). Imported by tests only.
import type {
  CameraFeatureDescriptor,
  CameraFeatureEntry,
  CameraFeatureReply,
  CameraFeatureSession,
  CameraFeatureTreeReply,
  CameraFeatureValue,
} from "../bridge";

export const SESSION: CameraFeatureSession = {
  instance: "4f1c2a9e-7b3d-4e21-9a0c-5d6e7f809a1b", source: "mock:mock_frames", generation: "1", capability_revision: "1",
};

const float = (value: number): CameraFeatureValue => ({ type: "float", value });
const str = (value: string): CameraFeatureValue => ({ type: "string", value });

function descriptor(over: Partial<CameraFeatureDescriptor>): CameraFeatureDescriptor {
  return {
    kind: "float", implemented: true, readable: true, writable: true, access_reason: "", visibility: "Beginner", unit: "",
    value: null, minimum: null, maximum: null, increment: null, entries: [], ...over,
  };
}

function feature(name: string, display: string, d: CameraFeatureDescriptor | null, error = ""): CameraFeatureEntry {
  return { name, display_name: display, tooltip: "", description: "", visibility: d?.visibility ?? "", descriptor: d, error };
}

export const FEATURES = {
  DeviceModelName: feature("DeviceModelName", "Device Model Name",
    descriptor({ kind: "string", writable: false, access_reason: "The node is read-only.", value: str("MIB Mock Camera") })),
  AcquisitionFrameRate: feature("AcquisitionFrameRate", "Acquisition Frame Rate",
    descriptor({ unit: "Hz", value: float(30), minimum: float(1), maximum: float(1000) })),
  ExposureAuto: feature("ExposureAuto", "Exposure Auto",
    descriptor({ kind: "enumeration", value: str("Off"), entries: ["Off", "Continuous"] })),
  ExposureTime: feature("ExposureTime", "Exposure Time",
    descriptor({ unit: "us", value: float(1000), minimum: float(10), maximum: float(1000000) })),
  TriggerSelector: feature("TriggerSelector", "Trigger Selector",
    descriptor({ kind: "enumeration", visibility: "Expert", value: str("FrameStart"), entries: ["FrameStart", "AcquisitionStart"] })),
  TriggerMode: feature("TriggerMode", "Trigger Mode",
    descriptor({ kind: "enumeration", visibility: "Expert", value: str("Off"), entries: ["Off", "On"] })),
  Gain: feature("Gain", "Gain", descriptor({ unit: "dB", value: float(0), minimum: float(0), maximum: float(24) })),
  ReverseX: feature("ReverseX", "Reverse X", descriptor({ kind: "boolean", visibility: "Expert", value: { type: "boolean", value: false } })),
};

/** A read tree of the remote module; `features` replaces the feature list (categories keep their names). */
export function treeReply(over: { features?: CameraFeatureEntry[]; session?: CameraFeatureSession } = {}): CameraFeatureTreeReply {
  const features = over.features ?? Object.values(FEATURES);
  const names = new Set(features.map((f) => f.name));
  const keep = (list: string[]) => list.filter((n) => names.has(n));
  return {
    schema_version: 1, outcome: "read", reason: "", evidence: "fixture/mock", session: over.session ?? SESSION,
    modules: [{
      module: "remote", enumerated: true, reason: "", truncated: false, roots: ["Root"],
      categories: [
        { name: "Root", display_name: "Root", tooltip: "Mock camera features", description: "", visibility: "Beginner",
          categories: ["DeviceControl", "AcquisitionControl", "AnalogControl"], features: [], error: "" },
        { name: "DeviceControl", display_name: "Device Control", tooltip: "", description: "", visibility: "Beginner", categories: [],
          features: keep(["DeviceModelName"]), error: "" },
        { name: "AcquisitionControl", display_name: "Acquisition Control", tooltip: "", description: "", visibility: "Beginner", categories: [],
          features: keep(["AcquisitionFrameRate", "ExposureAuto", "ExposureTime", "TriggerSelector", "TriggerMode"]), error: "" },
        { name: "AnalogControl", display_name: "Analog Control", tooltip: "", description: "", visibility: "Beginner", categories: [],
          features: keep(["Gain", "ReverseX"]), error: "" },
      ],
      features,
      uncategorized: [],
    }],
  };
}

/** A tree reply that is not "read" (no camera session, no GenApi, the session moved mid-walk). */
export function treeOutcome(outcome: CameraFeatureTreeReply["outcome"], reason: string): CameraFeatureTreeReply {
  return { schema_version: 1, outcome, reason, evidence: "", session: outcome === "stale" ? SESSION : null, modules: [] };
}

/** A camera_feature_write reply as the backend shapes it. */
export function writeReply(outcome: CameraFeatureReply["outcome"], feature: string, requested: CameraFeatureValue,
  readback: CameraFeatureDescriptor | null, reason = "", session: CameraFeatureSession | null = SESSION): CameraFeatureReply {
  return { schema_version: 1, outcome, reason, evidence: "fixture/mock", session, address: { module: "remote", feature, selector: null },
    requested, readback };
}
