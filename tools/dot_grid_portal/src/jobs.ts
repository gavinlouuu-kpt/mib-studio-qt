import { loadInspection } from "./inspection";
import { type Job, type Design, FILES } from "./api";
async function checked(r: Response) {
  if (!r.ok) {
    const b = await r.text();
    throw new Error(b.slice(0, 1000));
  }
  return r;
}
export async function execute(job: Job, attempt: number, env: Env) {
  if (
    !/^dg-[0-9a-f-]+$/.test(job.id) ||
    !["inspect", "generate"].includes(job.phase)
  )
    throw new Error("Invalid queue message");
  const d = await env.DB.prepare("SELECT * FROM designs WHERE id=?")
    .bind(job.id)
    .first<Design>();
  if (!d) return;
  const states =
    job.phase === "inspect"
      ? ["queued_inspect", "inspecting"]
      : ["queued_generate", "generating"];
  if (!states.includes(d.status)) return;
  await env.DB.prepare(
    "UPDATE designs SET status=?,updated_at=CURRENT_TIMESTAMP WHERE id=?",
  )
    .bind(job.phase === "inspect" ? "inspecting" : "generating", d.id)
    .run();
  const c = env.CAD.getByName(`${d.id}-${job.phase}-${attempt}`);
  const prefix = `${d.tenant_id}/${d.id}`;
  const source = await env.FILES.get(
    `${prefix}/${job.phase === "inspect" ? `source.${d.extension}` : "normalized.dxf"}`,
  );
  if (!source) throw new Error("Source file unavailable");
  await checked(
    await c.fetch(
      new Request("http://cad/input", {
        method: "PUT",
        body: source.body,
        headers: {
          "Content-Type": "application/octet-stream",
          "X-Extension": job.phase === "inspect" ? d.extension : "dxf",
        },
      }),
    ),
  );
  if (job.phase === "inspect") {
    const r = await checked(
      await c.fetch("http://cad/inspect", {
        method: "POST",
        body: JSON.stringify({ units: d.units }),
        headers: { "Content-Type": "application/json" },
      }),
    );
    const inspection = await r.json<any>();
    const serialized = JSON.stringify(inspection);
    let record = serialized;
    if (serialized.length > 256000) {
      await env.FILES.put(`${prefix}/inspection.json`, serialized);
      record = JSON.stringify({
        geometry_object: true,
        no_dot_contract: inspection.no_dot_contract,
        selected_regions: [],
      });
    }
    const normalized = await checked(
      await c.fetch("http://cad/files/normalized.dxf"),
    );
    await env.FILES.put(`${prefix}/normalized.dxf`, normalized.body);
    await env.DB.prepare(
      "UPDATE designs SET status='awaiting_confirmation',inspection=?,error=NULL,updated_at=CURRENT_TIMESTAMP WHERE id=?",
    )
      .bind(record, d.id)
      .run();
  } else {
    // One queue consumer serializes registration, including cross-design validation.
    const registered = await env.DB.prepare(
      "SELECT registry_entry FROM designs WHERE registry_entry IS NOT NULL",
    ).all<{ registry_entry: string }>();
    const r = await checked(
      await c.fetch("http://cad/generate", {
        method: "POST",
        body: JSON.stringify({
          id: d.id,
          name: d.name,
          seed: d.seed,
          inspection: await loadInspection(env, d),
          designs: registered.results.map((x) => JSON.parse(x.registry_entry)),
        }),
        headers: { "Content-Type": "application/json" },
      }),
    );
    const result = await r.json<{
      entry: Record<string, unknown>;
      preview?: Record<string, unknown>;
    }>();
    for (const f of FILES) {
      const file = await checked(await c.fetch(`http://cad/files/${f}`));
      await env.FILES.put(`${prefix}/${f}`, file.body);
    }
    if (result.preview)
      await env.FILES.put(
        `${prefix}/dot-preview.json`,
        JSON.stringify(result.preview),
      );
    await env.DB.prepare(
      "UPDATE designs SET status='ready',registry_entry=?,error=NULL,updated_at=CURRENT_TIMESTAMP WHERE id=?",
    )
      .bind(JSON.stringify(result.entry), d.id)
      .run();
  }
  await c.stop();
}
