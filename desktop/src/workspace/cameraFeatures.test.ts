// #806 E-3 (goal E02): quick controls are SFNC names resolved against the camera's own node map.
// The binding table only names what to look up; the tree reply decides absent, read-only and
// writable, and a write reply is shown as the backend returned it. Shapes: bridge-contract.json
// camera_features (the fixture mirrors the mock camera's fake node map).
import { describe, expect, it } from "vitest";
import {
  encodeDraft,
  NOT_AVAILABLE,
  rangeText,
  resolutionStatus,
  resolveBinding,
  SFNC_BINDINGS,
  treeLines,
  writeSummary,
} from "./cameraFeatures";
import { FEATURES, SESSION, treeOutcome, treeReply, writeReply } from "./cameraFeatureFixtures";

describe("the SFNC binding table", () => {
  it("names only standard features to look up; it carries no access, type or range", () => {
    expect(SFNC_BINDINGS.map((b) => b.feature)).toEqual(["ExposureTime", "Gain", "AcquisitionFrameRate", "TriggerMode"]);
    for (const b of SFNC_BINDINGS) expect(Object.keys(b).sort()).toEqual(["feature", "id", "label", "module"]);
  });
});

describe("resolveBinding against the live tree", () => {
  it("writable: the node is implemented and writable now; the tree's token is carried for the write", () => {
    const r = resolveBinding(treeReply(), "remote", "ExposureTime");
    expect(r.state).toBe("writable");
    if (r.state !== "writable") throw new Error();
    expect(r.session).toEqual(SESSION);
    expect(r.descriptor.unit).toBe("us");
    expect(rangeText(r.descriptor)).toBe("10–1000000 us");
    expect(resolutionStatus(r)).toMatchObject({ tone: "neutral", label: "Writable" });
  });

  it("absent: a name the node map does not list is not available on this camera, whatever the table says", () => {
    const tree = treeReply({ features: [FEATURES.ExposureTime, FEATURES.AcquisitionFrameRate] });
    const r = resolveBinding(tree, "remote", "Gain");
    expect(r).toEqual({ state: "absent", reason: NOT_AVAILABLE });
    expect(resolutionStatus(r)).toMatchObject({ tone: "unsupported", label: "Not available on this camera" });
    const unimplemented = treeReply({ features: [{ ...FEATURES.Gain, descriptor: { ...FEATURES.Gain.descriptor!, implemented: false } }] });
    expect(resolveBinding(unimplemented, "remote", "Gain")).toEqual({ state: "absent", reason: `${NOT_AVAILABLE} (not implemented)` });
    const truncated = treeReply({ features: [] });
    truncated.modules[0].truncated = true;
    expect(resolveBinding(truncated, "remote", "Gain").state).toBe("absent");
  });

  it("read-only: the node exists but is not writable now, with the device's access reason", () => {
    // The mock's ExposureTime is writable only while ExposureAuto is Off: the backend re-read it as read-only.
    const ro = { ...FEATURES.ExposureTime, descriptor: { ...FEATURES.ExposureTime.descriptor!, writable: false,
      access_reason: "Not writable while ExposureAuto is Continuous." } };
    const r = resolveBinding(treeReply({ features: [ro] }), "remote", "ExposureTime");
    expect(r).toMatchObject({ state: "readOnly", reason: "Not writable while ExposureAuto is Continuous." });
    expect(resolutionStatus(r)).toEqual({ tone: "neutral", label: "Read-only · Not writable while ExposureAuto is Continuous.",
      title: "Read-only: Not writable while ExposureAuto is Continuous." });
    const quiet = { ...ro, descriptor: { ...ro.descriptor, access_reason: "" } };
    expect(resolveBinding(treeReply({ features: [quiet] }), "remote", "ExposureTime")).toMatchObject({ state: "readOnly", reason: "read-only on this camera now" });
  });

  it("stale: a tree whose session moved mid-walk resolves nothing and asks for a re-read", () => {
    const r = resolveBinding(treeOutcome("stale", "Source, session or capability revision changed during enumeration; enumerate again."),
      "remote", "ExposureTime");
    expect(r.state).toBe("stale");
    expect(resolutionStatus(r)).toMatchObject({ tone: "warn", label: "Stale · reading the tree again" });
  });

  it("unknown: no tree yet, no camera session, a feature the backend could not describe", () => {
    expect(resolveBinding(null, "remote", "Gain")).toEqual({ state: "unknown", reason: "the feature tree has not been read yet" });
    expect(resolveBinding(treeOutcome("refused", "No camera session is open; start capture first."), "remote", "Gain"))
      .toEqual({ state: "unknown", reason: "No camera session is open; start capture first." });
    const failed = { ...FEATURES.Gain, descriptor: null, error: "GenTL error -1010: access denied" };
    expect(resolveBinding(treeReply({ features: [failed] }), "remote", "Gain")).toEqual({ state: "unknown", reason: "GenTL error -1010: access denied" });
    const notEnumerated = treeReply();
    notEnumerated.modules[0] = { ...notEnumerated.modules[0], enumerated: false, reason: "This port does not enumerate its node map." };
    expect(resolveBinding(notEnumerated, "remote", "Gain")).toEqual({ state: "unknown", reason: "This port does not enumerate its node map." });
  });

  it("unsupported: a camera without a GenApi node map", () => {
    const r = resolveBinding(treeOutcome("unsupported", "The active camera has no GenApi node map."), "remote", "Gain");
    expect(r).toEqual({ state: "unsupported", reason: "The active camera has no GenApi node map." });
  });
});

describe("a write reply is shown as the backend returned it", () => {
  const requested = { type: "float", value: 250 } as const;
  it("readback: requested and readback stay apart", () => {
    const rb = { ...FEATURES.ExposureTime.descriptor!, value: { type: "float" as const, value: 248 } };
    expect(writeSummary(writeReply("readback", "ExposureTime", requested, rb)))
      .toEqual({ tone: "confirmed", label: "Written · readback", requested: "250 us", readback: "248 us", reason: "" });
  });
  it("refused, stale and unknown outcome keep the backend's reason", () => {
    expect(writeSummary(writeReply("refused", "ExposureTime", requested, FEATURES.ExposureTime.descriptor!, "Value is outside the current device bounds.")))
      .toMatchObject({ tone: "blocked", label: "Refused", reason: "Value is outside the current device bounds." });
    expect(writeSummary(writeReply("stale", "ExposureTime", requested, null, "Backend instance, source, session or capability revision changed; refresh before retrying.")))
      .toMatchObject({ tone: "warn", label: "Stale", readback: "no readback" });
    expect(writeSummary(writeReply("unknown_write_outcome", "ExposureTime", requested, null, "write transport outcome unknown")))
      .toMatchObject({ tone: "unknown", label: "Unknown outcome · it may have reached the camera", readback: "no readback" });
    expect(writeSummary(writeReply("written_unconfirmed", "ExposureTime", requested, null))).toMatchObject({ tone: "warn", label: "Written · no readback" });
  });
});

describe("drafts are encoded by the node's kind, never range-checked here", () => {
  it("integers stay decimal strings; floats are numbers; enums are strings; out-of-range goes to the backend", () => {
    const int = { ...FEATURES.Gain.descriptor!, kind: "integer" as const };
    expect(encodeDraft(int, " 9007199254740993 ")).toEqual({ ok: true, value: { type: "integer", value: "9007199254740993" } });
    expect(encodeDraft(int, "1.5")).toEqual({ ok: false, reason: "an integer is required" });
    expect(encodeDraft(FEATURES.Gain.descriptor!, "99")).toEqual({ ok: true, value: { type: "float", value: 99 } }); // max 24: the backend refuses
    expect(encodeDraft(FEATURES.TriggerMode.descriptor!, "On")).toEqual({ ok: true, value: { type: "string", value: "On" } });
    expect(encodeDraft(FEATURES.ReverseX.descriptor!, "true")).toEqual({ ok: true, value: { type: "boolean", value: true } });
  });
});

describe("the read-only tree browser", () => {
  it("lists categories depth first in node-map order, then their features", () => {
    const lines = treeLines(treeReply().modules[0]);
    expect(lines.map((l) => `${"  ".repeat(l.depth)}${l.kind === "category" ? "+" : "-"}${l.name}`)).toEqual([
      "+Root", "  +DeviceControl", "    -DeviceModelName", "  +AcquisitionControl", "    -AcquisitionFrameRate", "    -ExposureAuto",
      "    -ExposureTime", "    -TriggerSelector", "    -TriggerMode", "  +AnalogControl", "    -Gain", "    -ReverseX",
    ]);
  });
  it("survives a node map that repeats or cycles a category", () => {
    const m = treeReply().modules[0];
    m.categories[1] = { ...m.categories[1], categories: ["Root"] };
    expect(treeLines(m).filter((l) => l.name === "Root")).toHaveLength(1);
  });
});
