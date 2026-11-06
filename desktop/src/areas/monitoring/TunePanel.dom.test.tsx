// @vitest-environment jsdom
// The tune panel and its hook together (#866 MON-069..081): the fields, the draft markers and footer,
// Apply to the running configuration and to an open file, Revert, conflicts, and the experiment's rules.
import { act, useCallback, useReducer } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { EXPERIMENT_STATES } from "../../bridgeContract";
import type { ConfigDocument } from "../../configDocument";
import { StudioContext } from "../../studio/StudioContext";
import type { EmodulusTableState } from "./emodulusLut";
import { TunePanel } from "./TunePanel";
import { initialTuneState, tuneReducer, type TuneFieldId } from "./tuneDraft";
import { TUNE_SYNC_MS, useTunePanel } from "./useTune";

vi.mock("@tauri-apps/api/core", () => ({ invoke: vi.fn() }));
vi.mock("../../bridge", () => ({
  bridge: { fetchProcessingConfigJson: vi.fn(), applyProcessingTune: vi.fn() },
}));
vi.mock("../../configDocument", () => ({ configDocument: { apply: vi.fn(), read: vi.fn() } }));
import { bridge } from "../../bridge";
import { configDocument } from "../../configDocument";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

const imageProcessing = () => ({
  gaussian_blur_size: 3, area_threshold_min: 60, area_threshold_max: 290, deformability_threshold_min: 0, deformability_threshold_max: 1,
  area_ratio_threshold_max: 1.5, ring_ratio_min: 15, ring_ratio_max: 25,
  filters: { enable_border_check: true, enable_area_range_check: true, enable_deformability_range_check: false, enable_area_ratio_check: false,
    enable_ring_ratio_check: true, enable_laplacian_variance_check: false, require_single_inner_contour: true },
  target_group: { enabled: false, area_min: 72, area_max: 191, deformability_min: 0, deformability_max: 0.3, emodulus_enabled: false, emodulus_min: 0, emodulus_max: 10 },
  multi_image: { enabled: false, count: 1 },
});
type Section = ReturnType<typeof imageProcessing>;
let running: Section;
const liveDocument = () => ({ valid: true, json: JSON.stringify({ config_version: 7, image_processing: running, realtime_processing: { enabled: true } }) });
const file = (section: unknown, revision = "rev-1"): ConfigDocument => ({ ok: true, path: "/cfg/config.json", revision, document_json: JSON.stringify({ image_processing: section }), error: "" });

/** The backend's merge of an image_processing patch into the running section. */
function merge(target: Record<string, unknown>, patch: Record<string, unknown>) {
  for (const [key, value] of Object.entries(patch)) {
    if (value && typeof value === "object") merge(target[key] as Record<string, unknown>, value as Record<string, unknown>);
    else target[key] = value;
  }
}
const fakeApply = async (json: string) => { merge(running as never, JSON.parse(json).image_processing); return { ok: true, message: "Processing settings applied", command: 2, operation_id: "0" } as never; };
const fakeFileApply = async (_path: string, _baseline: string, patch: string) => {
  merge(running as never, JSON.parse(patch).image_processing);
  return { saved: true, applied: true, verified: true, conflict: false, revision: "rev-2", error: "" };
};

let expState: number;
let hostProcessing: boolean;
let checked: { doc: ConfigDocument | null; dirty: boolean; busy: boolean; adopt: ReturnType<typeof vi.fn> };
let monitoringVisible: boolean;
let emodulusTable: EmodulusTableState;
let append: ReturnType<typeof vi.fn>;
let refreshConfig: ReturnType<typeof vi.fn>;
let host: HTMLDivElement, root: Root;

function Harness() {
  const [tune, dispatchTune] = useReducer(tuneReducer, undefined, initialTuneState);
  const setTuneField = useCallback((field: TuneFieldId, value: unknown) => dispatchTune({ type: "edit", field, value }), []);
  const model = useTunePanel({ ready: true, append, hostProcessing, tune, dispatchTune, expState, refreshConfig, checkedConfig: checked as never, monitoringVisible });
  const studio = { tune, setTuneField, onTuneApply: model.onTuneApply, onTuneRevert: model.onTuneRevert, tuneScopeNow: model.tuneScopeNow, checkedConfig: checked, hostProcessing, emodulusTable };
  return <StudioContext.Provider value={studio as never}><TunePanel /></StudioContext.Provider>;
}
const render = async () => { await act(async () => root.render(<Harness />)); };
/** The sync interval is created when the panel mounts, so fake timers must be on before that. */
async function remountWithFakeTimers() {
  await act(async () => root.unmount());
  vi.useFakeTimers();
  root = createRoot(host);
  await render();
}

const q = (selector: string) => host.querySelector<HTMLElement>(selector)!;
const input = (label: string) => host.querySelector<HTMLInputElement>(`input[aria-label="${label}"]`)!;
const checkbox = (name: string) => [...host.querySelectorAll<HTMLElement>(".tune-criterion-title")].find((l) => l.textContent?.startsWith(name))!.querySelector("input")!;
const button = (name: string) => [...host.querySelectorAll("button")].find((b) => b.textContent === name)!;
const state = () => q('[data-testid="tune-state"]').textContent;
const note = () => q('[data-testid="tune-note"]').textContent;
async function type(label: string, value: string) {
  await act(async () => {
    const el = input(label);
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!.call(el, value);
    el.dispatchEvent(new Event("input", { bubbles: true }));
  });
}
async function blur(label: string) { await act(async () => { input(label).dispatchEvent(new FocusEvent("focusout", { bubbles: true })); }); }
async function click(element: HTMLElement) { await act(async () => element.click()); }

beforeEach(async () => {
  vi.resetAllMocks();
  running = imageProcessing();
  expState = EXPERIMENT_STATES.Idle; hostProcessing = true; monitoringVisible = true; emodulusTable = { status: "none" };
  checked = { doc: null, dirty: false, busy: false, adopt: vi.fn() };
  append = vi.fn(); refreshConfig = vi.fn().mockResolvedValue(undefined);
  vi.mocked(bridge.fetchProcessingConfigJson).mockImplementation(async () => liveDocument());
  vi.mocked(bridge.applyProcessingTune).mockImplementation(fakeApply);
  vi.mocked(configDocument.apply).mockImplementation(fakeFileApply);
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
  await render();
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); vi.useRealTimers(); });

describe("the panel", () => {
  it("shows the Qt layout: three headings, the criteria with units and the running values", () => {
    expect([...host.querySelectorAll("h5")].map((h) => h.textContent)).toEqual(["Cell acceptance filters", "Target group / sorting gate", "Multi-image acquisition"]);
    expect([...host.querySelectorAll(".tune-criterion-title")].map((l) => l.textContent)).toEqual([
      "Area (µm²)", "Deformability", "Ring ratio", "Area ratio", "Border exclusion", "Single inner contour", "Target group gate", "Young's modulus", "Record image series",
    ]);
    expect(input("Area minimum (µm²)").value).toBe("60");
    expect(input("Area maximum (µm²)").value).toBe("290");
    expect(input("Deformability maximum").value).toBe("1.00");
    expect(input("Ring ratio minimum").value).toBe("15.0");
    expect(input("Area ratio maximum").value).toBe("1.50");
    expect(input("Target group Young's modulus maximum (kPa)").value).toBe("10.00");
    expect(input("Images per trigger").value).toBe("1");
    expect(q('[aria-label="Area (µm²)"]').textContent).toContain("µm²");
    expect(q('[aria-label="Young\'s modulus"]').textContent).toContain("kPa");
    expect(checkbox("Area (µm²)").checked).toBe(true);
    expect(checkbox("Border exclusion").checked).toBe(true);
    expect(checkbox("Target group gate").checked).toBe(false);
    expect(host.textContent).toContain("An object must pass every enabled criterion to count as a valid cell.");
    expect(host.textContent).toContain("Applied to valid cells only: selects which of them fire the sort trigger. Never changes validity.");
    expect(host.textContent).toContain("Reject objects touching the ROI edge.");
    expect(host.textContent).toContain("No E-modulus table is loaded: this range is skipped and the target group is the area and deformability box alone.");
    expect(state()).toBe("Applied");
  });

  it("says whether the E-modulus table is loaded under the Young's modulus range (MON-077)", async () => {
    const hint = () => q('[data-testid="tune-emodulus-hint"]').textContent;
    expect(hint()).toBe("No E-modulus table is loaded: this range is skipped and the target group is the area and deformability box alone.");
    emodulusTable = { status: "loaded", table: { identity: "ab|/x", info: { path: "/data/lut.txt", sha256: "ab", lut_id: "lut-6.16", revision: "", source: "", catalog_sha256: "", checksum_status: "" }, lut: {} as never } };
    await render();
    expect(hint()).toBe("E-modulus table loaded (lut-6.16): this range filters the target group.");
    emodulusTable = { status: "unknown" };
    await render();
    expect(hint()).toBe("Checking for the E-modulus table…");
    expect(q('[data-tour="monitoring-tune-emodulus-table"]')).toBe(q('[data-testid="tune-emodulus-hint"]'));
  });

  it("gives number inputs the Qt range, step and decimals", () => {
    const area = input("Area maximum (µm²)");
    expect([area.min, area.max, area.step]).toEqual(["0", "100000", "10"]);
    const def = input("Deformability minimum");
    expect([def.min, def.max, def.step]).toEqual(["0", "1", "0.01"]);
    const ring = input("Ring ratio maximum");
    expect([ring.min, ring.max, ring.step]).toEqual(["0", "100", "0.5"]);
    const emod = input("Target group Young's modulus minimum (kPa)");
    expect([emod.min, emod.max, emod.step]).toEqual(["0", "1000", "0.1"]);
    const count = input("Images per trigger");
    expect([count.min, count.max, count.step]).toEqual(["1", "32", "1"]);
  });

  it("disables a switched-off criterion's value controls but keeps their values visible", async () => {
    expect(input("Deformability minimum").disabled).toBe(true);
    expect(input("Deformability maximum").value).toBe("1.00");
    expect(input("Area ratio maximum").disabled).toBe(true);
    expect(input("Area minimum (µm²)").disabled).toBe(false);
    expect(input("Target group area minimum (µm²)").disabled).toBe(true);
    expect(checkbox("Young's modulus").disabled).toBe(true); // nested in the target group, which is off
    await click(checkbox("Target group gate"));
    expect(input("Target group area minimum (µm²)").disabled).toBe(false);
    expect(checkbox("Young's modulus").disabled).toBe(false);
    expect(input("Target group Young's modulus minimum (kPa)").disabled).toBe(true); // its own box is off
    await click(checkbox("Young's modulus"));
    expect(input("Target group Young's modulus minimum (kPa)").disabled).toBe(false);
    await click(checkbox("Area (µm²)"));
    expect(input("Area minimum (µm²)").disabled).toBe(true);
    expect(input("Area minimum (µm²)").value).toBe("60");
  });

  it("carries a stable tour anchor on every control and part", () => {
    const anchors = [...host.querySelectorAll("[data-tour]")].map((e) => e.getAttribute("data-tour"));
    for (const expected of [
      "monitoring-tune-panel", "monitoring-tune-area", "monitoring-tune-area-min", "monitoring-tune-area-max", "monitoring-tune-deformability",
      "monitoring-tune-ring-ratio", "monitoring-tune-area-ratio", "monitoring-tune-border", "monitoring-tune-single-inner-contour",
      "monitoring-tune-target-group", "monitoring-tune-target-area-min", "monitoring-tune-target-emodulus", "monitoring-tune-target-emodulus-max",
      "monitoring-tune-multi-image", "monitoring-tune-multi-image-count", "monitoring-tune-footer", "monitoring-tune-apply", "monitoring-tune-revert",
    ]) expect(anchors, expected).toContain(expected);
    expect(new Set(anchors).size).toBe(anchors.length);
  });

  it("keeps its controls inside the viewer gate", () => {
    expect(host.querySelectorAll("fieldset.viewer-gate input, fieldset.viewer-gate button").length).toBe(host.querySelectorAll("input, button").length);
  });
});

describe("the draft markers and footer", () => {
  it("marks a changed row with * and the applied value in its tooltip, and counts it", async () => {
    await type("Area minimum (µm²)", "80");
    expect(q('[data-tour="monitoring-tune-area-min"]').closest("label")!.textContent).toContain("Minimum *");
    expect(input("Area minimum (µm²)").closest("label")!.title).toBe("Area minimum (µm²) — unapplied change (applied value: 60)");
    expect(input("Area maximum (µm²)").closest("label")!.title).toBe("Area maximum (µm²)");
    expect(state()).toBe("1 unapplied change");
    expect(button("Apply changes").disabled).toBe(false);
    expect(button("Revert").disabled).toBe(false);
    await click(checkbox("Border exclusion"));
    expect(checkbox("Border exclusion").closest("label")!.textContent).toBe("Border exclusion *");
    expect(state()).toBe("2 unapplied changes");
  });

  it("treats re-typing the applied value as no change", async () => {
    await type("Area minimum (µm²)", "80");
    await type("Area minimum (µm²)", "60");
    expect(state()).toBe("Applied");
    expect(button("Apply changes").disabled).toBe(true);
    expect(button("Revert").disabled).toBe(true);
  });

  it("clamps what is typed to the range and shows the clamped value once the field loses focus", async () => {
    await type("Area maximum (µm²)", "250000");
    expect(input("Area maximum (µm²)").value).toBe("250000"); // still what is typed
    await blur("Area maximum (µm²)");
    expect(input("Area maximum (µm²)").value).toBe("100000");
    await type("Deformability maximum", "0.456");
    await blur("Deformability maximum");
    expect(input("Deformability maximum").value).toBe("0.46");
  });

  it("shows Invalid with the issues and blocks Apply", async () => {
    await type("Area minimum (µm²)", "400");
    expect(state()).toBe("Invalid: Area: minimum (400 µm²) exceeds maximum (290 µm²)");
    expect(q('[data-testid="tune-issues"]').textContent).toBe("Area: minimum (400 µm²) exceeds maximum (290 µm²)");
    expect(button("Apply changes").disabled).toBe(true);
    await type("Area minimum (µm²)", "100");
    expect(state()).toBe("1 unapplied change");
    expect(host.querySelector('[data-testid="tune-issues"]')).toBeNull();
  });

  it("goes back to the running values on Revert", async () => {
    await type("Area minimum (µm²)", "80");
    await click(button("Revert"));
    expect(input("Area minimum (µm²)").value).toBe("60");
    expect(state()).toBe("Applied");
  });
});

describe("Apply", () => {
  it("applies to the running configuration only when no file is open, sending only the changed keys, and says so", async () => {
    await type("Area minimum (µm²)", "80");
    await click(checkbox("Border exclusion"));
    await click(button("Apply changes"));
    expect(bridge.applyProcessingTune).toHaveBeenCalledTimes(1);
    expect(JSON.parse(vi.mocked(bridge.applyProcessingTune).mock.calls[0][0])).toEqual({
      image_processing: { area_threshold_min: 80, filters: { enable_border_check: false } },
    });
    expect(configDocument.apply).not.toHaveBeenCalled();
    // The backend read back the new values: the draft is clean again.
    expect(state()).toBe("Applied");
    expect(note()).toBe("Applied to the running configuration only. Not saved to a file: no configuration file is open (App config > Open Config…).");
    expect(append).toHaveBeenCalledWith(expect.stringContaining("2 changes applied (not saved to a file"));
    expect(refreshConfig).toHaveBeenCalled();
  });

  it("confirms by reading the running configuration back, and shows what it now says", async () => {
    await type("Area minimum (µm²)", "80");
    await click(button("Apply changes"));
    expect(input("Area minimum (µm²)").value).toBe("80");
    expect(state()).toBe("Applied");
  });

  it("saves to the open file when it is clean and agrees with the running configuration", async () => {
    checked.doc = file({ ...imageProcessing(), unknown_key: 1 });
    vi.mocked(configDocument.read).mockResolvedValue(file(running, "rev-2"));
    await render();
    await type("Area minimum (µm²)", "80");
    await click(button("Apply changes"));
    expect(configDocument.apply).toHaveBeenCalledWith("/cfg/config.json", "rev-1", JSON.stringify({ image_processing: { area_threshold_min: 80 } }));
    expect(bridge.applyProcessingTune).not.toHaveBeenCalled();
    expect(checked.adopt).toHaveBeenCalledWith(expect.objectContaining({ revision: "rev-2" }));
    expect(state()).toBe("Applied");
    expect(input("Area minimum (µm²)").value).toBe("80");
    expect(note()).toBe("Applied and saved to /cfg/config.json.");
    expect(append).toHaveBeenCalledWith(expect.stringContaining("saved to /cfg/config.json"));
  });

  it("applies to the running configuration only when the open file disagrees with it, naming the key", async () => {
    checked.doc = file({ ...imageProcessing(), gaussian_blur_size: 7 });
    await render();
    await type("Area minimum (µm²)", "80");
    await click(button("Apply changes"));
    expect(configDocument.apply).not.toHaveBeenCalled();
    expect(bridge.applyProcessingTune).toHaveBeenCalledTimes(1);
    expect(note()).toBe("Applied to the running configuration only. Not saved to a file: config.json differs from the running configuration (gaussian_blur_size).");
  });

  it("does not use a file that has unsaved edits in App config", async () => {
    checked.doc = file(imageProcessing()); checked.dirty = true;
    await render();
    await type("Area minimum (µm²)", "80");
    await click(button("Apply changes"));
    expect(configDocument.apply).not.toHaveBeenCalled();
    expect(note()).toContain("config.json has unsaved edits in the App config editor");
  });

  it("says Saved, not applied when the file took it and the running configuration did not", async () => {
    checked.doc = file(imageProcessing());
    vi.mocked(configDocument.apply).mockResolvedValue({ saved: true, applied: false, verified: false, conflict: false, revision: "rev-2", error: "Saved; runtime application failed" });
    vi.mocked(configDocument.read).mockResolvedValue(file(imageProcessing(), "rev-2"));
    await render();
    await type("Area minimum (µm²)", "80");
    await click(button("Apply changes"));
    expect(state()).toBe("Saved, not applied: Saved; runtime application failed");
    expect(input("Area minimum (µm²)").value).toBe("80"); // the draft is kept
    expect(checked.adopt).toHaveBeenCalled(); // the file did change
    expect(button("Apply changes").disabled).toBe(false);
  });

  it("is a Conflict when the file changed since it was opened: nothing is written, the draft is kept until Revert", async () => {
    checked.doc = file(imageProcessing());
    vi.mocked(configDocument.apply).mockResolvedValue({ saved: false, applied: false, verified: false, conflict: true, revision: "rev-9", error: "changed" });
    await render();
    await type("Area minimum (µm²)", "80");
    await click(button("Apply changes"));
    expect(state()).toMatch(/^Conflict:/);
    expect(input("Area minimum (µm²)").value).toBe("80");
    expect(button("Apply changes").disabled).toBe(true);
    // Revert reads the file again (the editor gets the new revision) and the running configuration.
    vi.mocked(configDocument.read).mockResolvedValue(file(imageProcessing(), "rev-9"));
    await click(button("Revert"));
    expect(configDocument.read).toHaveBeenCalledWith("/cfg/config.json");
    expect(checked.adopt).toHaveBeenCalledWith(expect.objectContaining({ revision: "rev-9" }));
    expect(input("Area minimum (µm²)").value).toBe("60");
    expect(state()).toBe("Applied");
  });

  it("shows the backend's refusal and keeps the draft", async () => {
    vi.mocked(bridge.applyProcessingTune).mockResolvedValue({ ok: false, message: "Processing settings are locked during an experiment: image_processing.multi_image cannot change until it ends", command: 2, operation_id: "0" } as never);
    await type("Area minimum (µm²)", "80");
    await click(button("Apply changes"));
    expect(state()).toBe("Not applied: Processing settings are locked during an experiment: image_processing.multi_image cannot change until it ends");
    expect(input("Area minimum (µm²)").value).toBe("80");
    expect(append).toHaveBeenCalledWith(expect.stringContaining("Tune: not applied"));
  });

  it("does not claim success when the read-back differs", async () => {
    // The backend says ok but the running configuration still has the old value.
    vi.mocked(bridge.applyProcessingTune).mockResolvedValue({ ok: true, message: "ok", command: 2, operation_id: "0" } as never);
    await type("Area minimum (µm²)", "80");
    await click(button("Apply changes"));
    expect(state()).toBe("Not applied: the processing service reports different values than were applied");
    expect(input("Area minimum (µm²)").value).toBe("80");
  });

  it("is a Conflict, with nothing sent, when the running configuration moved under the draft", async () => {
    await type("Area minimum (µm²)", "80");
    running.ring_ratio_max = 40;
    await click(button("Apply changes"));
    expect(bridge.applyProcessingTune).not.toHaveBeenCalled();
    expect(state()).toMatch(/^Conflict:/);
    expect(input("Area minimum (µm²)").value).toBe("80");
    await click(button("Revert"));
    expect(input("Ring ratio maximum").value).toBe("40.0");
    expect(input("Area minimum (µm²)").value).toBe("60");
  });

  it("reports a bridge failure and keeps the draft", async () => {
    vi.mocked(bridge.applyProcessingTune).mockRejectedValue(new Error("bridge disconnected"));
    await type("Area minimum (µm²)", "80");
    await click(button("Apply changes"));
    expect(state()).toContain("bridge disconnected");
    expect(input("Area minimum (µm²)").value).toBe("80");
  });
});

describe("the experiment", () => {
  it("applies the criteria live during a run and keeps multi-image locked", async () => {
    expState = EXPERIMENT_STATES.Active;
    await render();
    expect(input("Images per trigger").disabled).toBe(true);
    expect(checkbox("Record image series").disabled).toBe(true);
    expect(q('[data-testid="tune-multi-locked"]').textContent).toBe("Locked while an experiment runs: it fixes the run's saved image layout.");
    expect(note()).toContain("changes apply live");
    await type("Area minimum (µm²)", "80");
    expect(button("Apply changes").disabled).toBe(false);
    await click(button("Apply changes"));
    expect(state()).toBe("Applied");
    expect(append).toHaveBeenCalledWith(expect.stringContaining("applied to the running experiment"));
  });

  it("holds a multi-image change made before the run until the run ends", async () => {
    await click(checkbox("Record image series"));
    expect(state()).toBe("1 unapplied change");
    expState = EXPERIMENT_STATES.Active;
    await render();
    expect(button("Apply changes").disabled).toBe(true);
    expect(note()).toMatch(/^Multi-image changes wait until the experiment ends/);
    expState = EXPERIMENT_STATES.Idle;
    await render();
    expect(button("Apply changes").disabled).toBe(false);
  });

  it.each([
    [EXPERIMENT_STATES.Starting, /starting/], [EXPERIMENT_STATES.Stopping, /stopping/], [EXPERIMENT_STATES.Failed, /failed/],
  ])("holds Apply back in state %i and says why, while editing and Revert still work", async (state_, reason) => {
    await type("Area minimum (µm²)", "80");
    expState = state_;
    await render();
    expect(button("Apply changes").disabled).toBe(true);
    expect(button("Apply changes").title).toMatch(reason);
    expect(note()).toMatch(reason);
    expect(button("Revert").disabled).toBe(false);
    await type("Area maximum (µm²)", "300");
    expect(state()).toBe("2 unapplied changes");
    expect(bridge.applyProcessingTune).not.toHaveBeenCalled();
  });

  it("says on a PL instrument which settings the PL does not implement", async () => {
    expect(host.querySelector('[data-testid="tune-pl-note"]')).toBeNull();
    hostProcessing = false;
    await render();
    expect(q('[data-testid="tune-pl-note"]').textContent).toContain("The PL runs the science on this instrument. It does not implement gaussian blur");
    expect(q('[data-testid="tune-pl-note"]').textContent).toContain("the border check, the ring-ratio gate, the single-inner-contour requirement");
  });

  it("holds Apply back during a run on a PL instrument", async () => {
    hostProcessing = false; expState = EXPERIMENT_STATES.Active;
    await render();
    await type("Area minimum (µm²)", "80");
    expect(button("Apply changes").disabled).toBe(true);
    expect(note()).toMatch(/PL runs the science/);
  });
});

describe("following the running configuration", () => {
  it("reads it when Monitoring is shown and not while it is hidden", async () => {
    expect(bridge.fetchProcessingConfigJson).toHaveBeenCalledTimes(1);
    vi.mocked(bridge.fetchProcessingConfigJson).mockClear();
    monitoringVisible = false;
    await remountWithFakeTimers();
    await act(async () => { await vi.advanceTimersByTimeAsync(TUNE_SYNC_MS * 3); });
    expect(bridge.fetchProcessingConfigJson).not.toHaveBeenCalled();
  });

  it("refreshes silently from a change elsewhere while the draft is clean", async () => {
    await remountWithFakeTimers();
    running.area_threshold_min = 70;
    await act(async () => { await vi.advanceTimersByTimeAsync(TUNE_SYNC_MS); });
    expect(input("Area minimum (µm²)").value).toBe("70");
    expect(state()).toBe("Applied");
  });

  it("keeps a dirty draft and shows Conflict when a field it exposes changed elsewhere", async () => {
    await remountWithFakeTimers();
    await type("Area minimum (µm²)", "80");
    running.ring_ratio_max = 40;
    await act(async () => { await vi.advanceTimersByTimeAsync(TUNE_SYNC_MS); });
    expect(state()).toMatch(/^Conflict:/);
    expect(input("Area minimum (µm²)").value).toBe("80");
    expect(input("Ring ratio maximum").value).toBe("25.0"); // the draft's, until Revert
    expect(button("Apply changes").disabled).toBe(true);
    await click(button("Revert"));
    expect(input("Ring ratio maximum").value).toBe("40.0");
    expect(state()).toBe("Applied");
  });

  it("ignores a change to something the panel does not expose", async () => {
    await remountWithFakeTimers();
    await type("Area minimum (µm²)", "80");
    running.gaussian_blur_size = 5;
    await act(async () => { await vi.advanceTimersByTimeAsync(TUNE_SYNC_MS); });
    expect(state()).toBe("1 unapplied change");
  });

  it("stays read-only and waiting while the running configuration cannot be read", async () => {
    await act(async () => root.unmount());
    root = createRoot(host);
    vi.mocked(bridge.fetchProcessingConfigJson).mockResolvedValue({ valid: false, json: "" });
    await render();
    expect(state()).toBe("Waiting for the running configuration…");
    expect(input("Area minimum (µm²)").disabled).toBe(true);
    expect(button("Apply changes").disabled).toBe(true);
  });
});
