import type { Design } from "./api";

// D1 rows are bounded. Large wafer geometry belongs in tenant-private object
// storage, not in a multi-megabyte SQL value. Only decisions stay in D1.
export async function loadInspection(env: Env, d: Design): Promise<any> {
  const saved = JSON.parse(d.inspection || "null");
  if (!saved?.geometry_object) return saved;
  const object = await env.FILES.get(`${d.tenant_id}/${d.id}/inspection.json`);
  if (!object) throw new Error("Inspection geometry unavailable");
  const geometry = await new Response(object.body).json<
    Record<string, unknown>
  >();
  return { ...geometry, selected_regions: saved.selected_regions ?? [] };
}
export function inspectionRecord(previous: string | null, inspection: any) {
  const saved = JSON.parse(previous || "null");
  return JSON.stringify(
    saved?.geometry_object
      ? { ...saved, selected_regions: inspection.selected_regions ?? [] }
      : inspection,
  );
}
