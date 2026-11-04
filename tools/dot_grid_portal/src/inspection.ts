import type { Design } from "./api";

// D1 rows are bounded. Large wafer geometry belongs in tenant-private object
// storage, not in a multi-megabyte SQL value. Only decisions stay in D1.
export async function loadInspection(env: Env, d: Design): Promise<any> {
  const saved = JSON.parse(d.inspection || "null");
  let preview;
  if (d.status === "ready") {
    const object = await env.FILES.get(
      `${d.tenant_id}/${d.id}/dot-preview.json`,
    );
    if (object) preview = await new Response(object.body).json();
  }
  if (!saved?.geometry_object)
    return saved ? { ...saved, generated_preview: preview } : saved;
  const object = await env.FILES.get(`${d.tenant_id}/${d.id}/inspection.json`);
  if (!object) throw new Error("Inspection geometry unavailable");
  const geometry = await new Response(object.body).json<
    Record<string, unknown>
  >();
  return {
    ...geometry,
    selected_regions: saved.selected_regions ?? [],
    chip_regions: saved.chip_regions ?? [],
    manual_chip_rectangles: saved.manual_chip_rectangles ?? [],
    cut_clearance_um: saved.cut_clearance_um ?? 200,
    generated_preview: preview,
  };
}
export function inspectionRecord(previous: string | null, inspection: any) {
  const saved = JSON.parse(previous || "null");
  return JSON.stringify(
    saved?.geometry_object
      ? {
          ...saved,
          selected_regions: inspection.selected_regions ?? [],
          chip_regions: inspection.chip_regions ?? [],
          manual_chip_rectangles: inspection.manual_chip_rectangles ?? [],
          cut_clearance_um: inspection.cut_clearance_um ?? 200,
          placement_contract: inspection.placement_contract,
        }
      : inspection,
  );
}
