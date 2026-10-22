import { describe, expect, it } from "vitest";
import { annotationExport, annotationGroups, annotationLayers } from "./annotationModel";
import { buildStartRequest, type PlanDraft } from "./planModel";
import fixture from "./fixtures/ique-annotations-v1.json";

const makePlan = (): PlanDraft => JSON.parse(JSON.stringify(fixture.plan));

describe("planned annotation projection (I01, not scientific evaluation)", () => {
  it("groups three samples in two conditions without inventing independent replicates", () => {
    const p = makePlan(), before = JSON.stringify(p);
    const groups = annotationGroups(p.rows, annotationLayers(p.rows)[0]);
    expect(groups[0]).toMatchObject({ value: "Vehicle", sampleRefs: ["S1", "S3"], declaredUnits: ["D1"], missingUnitSamples: 0, conditionSlot: 1 });
    expect(groups[1]).toMatchObject({ value: "Drug", sampleRefs: ["S2"], declaredUnits: ["D2"], conditionSlot: 2 });
    expect(JSON.stringify(p)).toBe(before);
  });
  it("namespaces free layers and trims keys consistently with the start contract", () => {
    const p = makePlan();
    p.rows[0].free.push({ key: "condition", value: "not the condition" });
    const layers = annotationLayers(p.rows);
    expect(layers.filter(l => l.freeKey === "batch")).toHaveLength(1);
    expect(layers.find(l => l.freeKey === "condition")?.id).toBe("free:condition");
    expect(annotationGroups(p.rows, layers.find(l => l.id === "free:batch")!)[0].sampleRefs).toEqual(["S1", "S3"]);
  });
  it("keeps absent values separate from literal missing labels and preserves missing units", () => {
    const p = makePlan();
    p.rows[0].condition = ""; p.rows[0].experimentalUnit = "";
    p.rows[1].condition = "Not declared / invalid";
    const groups = annotationGroups(p.rows, annotationLayers(p.rows)[0]);
    expect(groups[0]).toMatchObject({ key: "null", value: null, missingUnitSamples: 1 });
    expect(groups[1].key).not.toBe(groups[0].key);
  });
  it("rejects duplicate free keys instead of silently choosing a category", () => {
    const p = makePlan();
    p.rows[0].free.push({ key: "batch", value: "B" });
    const layer = annotationLayers(p.rows).find(l => l.id === "free:batch")!;
    expect(annotationGroups(p.rows, layer)[0].value).toBeNull();
    expect(annotationExport(p, layer.id).json).toBeNull();
  });
  it("round-trips a versioned annotation export identical to the real Start annotation", () => {
    const p = makePlan();
    p.rows[0].notes = 'First line\nQuoted "note"';
    const result = JSON.parse(annotationExport(p, "free:batch").json!);
    expect(result.version).toBe(1);
    expect(result.experiment_id).toBe(p.experimentId);
    expect(result.selected_layer).toBe("free:batch");
    result.samples.forEach((s: { annotation: unknown; run_index?: number }, i: number) => {
      expect(s.annotation).toEqual(JSON.parse(buildStartRequest(p, i, i + 1).json!).annotation);
      expect(s.run_index).toBeUndefined();
    });
    expect(result.samples[0].annotation.concentration.value).toBe(0);
    expect(result.samples[0].annotation.notes).toBe(p.rows[0].notes);
  });
  it("refuses missing concentration units rather than exporting a scientific-looking number", () => {
    const p = makePlan(); p.rows[1].concentrationUnit = "";
    expect(annotationExport(p, "condition")).toMatchObject({ json: null });
    expect(annotationExport(p, "condition").issues.join(" ")).toContain("unit");
  });
  it("refuses invalid identities, empty sheets, duplicate refs and oversized text", () => {
    const p = makePlan(); p.experimentId = "invalid";
    expect(annotationExport(p, "condition").json).toBeNull();
    expect(annotationExport({ ...makePlan(), rows: [] }, "condition").json).toBeNull();
    const duplicate = makePlan(); duplicate.rows[1].ref = " S1 ";
    expect(annotationExport(duplicate, "condition").issues.join(" ")).toContain("duplicate sample reference");
    const oversized = makePlan(); oversized.rows[0].sample = "µ".repeat(1000);
    expect(annotationExport(oversized, "condition").json).toBeNull();
  });
  it("falls back when a removed free layer was selected; preserves hostile text as data", () => {
    const p = makePlan(); p.rows[0].free = [{ key: "__proto__", value: "<script>=1+1</script>" }];
    const result = JSON.parse(annotationExport(p, "free:removed").json!);
    expect(result.selected_layer).toBe("condition");
    expect(result.samples[0].annotation.free.__proto__).toBe("<script>=1+1</script>");
  });
});
