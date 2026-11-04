import { loadInspection, inspectionRecord } from "./inspection";
import { type Member, HttpError, checkMutation } from "./auth";
export type Design = {
  id: string;
  seed: number;
  tenant_id: string;
  name: string;
  extension: string;
  units: string;
  status: string;
  inspection: string | null;
  registry_entry: string | null;
  error: string | null;
};
export type Job = { id: string; phase: "inspect" | "generate" };
export const FILES = [
  "design_with_dotgrid.dxf",
  "dotgrid_layer.gds",
  "mib-map.zip",
] as const;
const CUSTOMER_FILES = [
  "design_with_dotgrid.dxf",
  "dotgrid_layer.gds",
] as const;
const MAX_UPLOAD = 10 * 1024 * 1024;
export function importError(error: string | null) {
  if (!error) return null;
  for (const [code, message] of Object.entries({
    CAD_GEOMETRY_LOSS:
      "Some filled geometry could not be preserved during import. Your original drawing is retained; no dots were generated. Contact support with this design selected so its converter can be upgraded.",
    CAD_REGION_UNSUPPORTED:
      "This drawing includes a filled shape our importer cannot yet preserve. Your original drawing is retained; no dots were generated. Contact support with this design selected.",
    CAD_CONVERTER_UNAVAILABLE:
      "DWG import is temporarily unavailable. Your original drawing is retained. Retry when support restores the converter.",
    CAD_CONVERSION_FAILED:
      "Drawing conversion did not complete. Your original drawing is retained. Retry or contact support with this design selected.",
    CAD_TOPOLOGY_LIMIT:
      "This drawing exceeds the current geometry-review capacity. Your original is retained; contact support with this design selected.",
  }))
    if (error.includes(code)) return message;
  return "Processing failed. Review the drawing and retry inspection.";
}

async function body(req: Request) {
  const reader = req.body?.getReader();
  if (!reader) throw new HttpError(400, "Request body required");
  let size = 0;
  const chunks: Uint8Array[] = [];
  while (true) {
    const r = await reader.read();
    if (r.done) break;
    size += r.value.length;
    if (size > 65536) {
      await reader.cancel();
      throw new HttpError(413, "Request too large");
    }
    chunks.push(r.value);
  }
  try {
    return JSON.parse(
      new TextDecoder().decode(Buffer.concat(chunks)),
    ) as Record<string, unknown>;
  } catch {
    throw new HttpError(400, "Invalid JSON");
  }
}
function text(v: unknown, max = 120) {
  if (typeof v !== "string" || !v.trim() || v.length > max)
    throw new HttpError(400, "Invalid text field");
  return v.trim();
}
export async function owned(env: Env, id: string, member: Member) {
  const d = await env.DB.prepare(
    "SELECT * FROM designs WHERE id=? AND tenant_id=?",
  )
    .bind(id, member.tenant_id)
    .first<Design>();
  if (!d) throw new HttpError(404, "Design not found");
  return d;
}
export async function api(
  req: Request,
  env: Env,
  member: Member,
): Promise<Response> {
  checkMutation(req);
  const url = new URL(req.url);
  const path = url.pathname;
  if (path === "/api/me" && req.method === "GET") return Response.json(member);
  if (path === "/api/designs" && req.method === "GET")
    return Response.json(
      (
        await env.DB.prepare(
          "SELECT id,name,status,error,created_at,json_extract(inspection,'$.no_dot_contract') AS no_dot_contract FROM designs WHERE tenant_id=? ORDER BY created_at DESC LIMIT 100",
        )
          .bind(member.tenant_id)
          .all()
      ).results.map((row: any) => ({
        id: row.id,
        name: row.name,
        status:
          row.status === "ready" && row.no_dot_contract !== 1
            ? "needs_review"
            : row.status,
        error: importError(row.error),
        created_at: row.created_at,
      })),
    );
  if (path === "/api/members") {
    if (member.role !== "admin")
      throw new HttpError(403, "Administrator required");
    if (req.method === "GET")
      return Response.json(
        (
          await env.DB.prepare(
            "SELECT email,role,active FROM members WHERE tenant_id=?",
          )
            .bind(member.tenant_id)
            .all()
        ).results,
      );
    if (req.method === "POST") {
      const b = await body(req);
      const email = text(b.email, 254).toLowerCase();
      if (!/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(email))
        throw new HttpError(400, "Invalid email");
      // Never move an existing account to another tenant, and never let an invite elevate roles.
      const r = await env.DB.prepare(
        "INSERT INTO members(email,tenant_id,role) VALUES(?,?,'member') ON CONFLICT(email) DO NOTHING RETURNING email",
      )
        .bind(email, member.tenant_id)
        .first();
      if (!r)
        throw new HttpError(
          409,
          "Invitation cannot be created for this address",
        );
      return Response.json(
        {
          email,
          message:
            "Access granted. Share this portal URL with the invitee; no email was sent.",
        },
        { status: 201 },
      );
    }
    if (req.method === "PATCH") {
      const b = await body(req);
      const email = text(b.email, 254).toLowerCase();
      if (email === member.email)
        throw new HttpError(400, "Cannot revoke yourself");
      const r = await env.DB.prepare(
        "UPDATE members SET active=0 WHERE email=? AND tenant_id=? AND role='member' RETURNING email",
      )
        .bind(email, member.tenant_id)
        .first();
      if (!r) throw new HttpError(404, "Member not found");
      return Response.json({ revoked: true });
    }
  }
  if (path === "/api/feedback" && req.method === "POST") {
    const b = await body(req);
    await env.DB.prepare(
      "INSERT INTO feedback(tenant_id,email,text) VALUES(?,?,?)",
    )
      .bind(member.tenant_id, member.email, text(b.text, 2000))
      .run();
    return Response.json({ saved: true });
  }
  if (path === "/api/designs" && req.method === "POST") {
    const ext = url.searchParams.get("extension")?.toLowerCase();
    const units = url.searchParams.get("units");
    const name = text(url.searchParams.get("name"));
    if (
      !["dxf", "dwg"].includes(ext ?? "") ||
      !["um", "mm", "inch"].includes(units ?? "")
    )
      throw new HttpError(400, "Choose DXF/DWG and drawing units");
    const length = Number(req.headers.get("Content-Length"));
    if (!Number.isSafeInteger(length) || length < 1 || length > MAX_UPLOAD)
      throw new HttpError(
        413,
        "Upload must be 1 byte–10 MiB with Content-Length",
      );
    const quota = await env.DB.prepare(
      "SELECT COUNT(*) AS n FROM designs WHERE tenant_id=? AND created_at >= datetime('now','-1 day')",
    )
      .bind(member.tenant_id)
      .first<{ n: number }>();
    if ((quota?.n ?? 0) >= 20)
      throw new HttpError(429, "Daily design limit reached");
    const id = `dg-${crypto.randomUUID()}`;
    let received = 0;
    if (!req.body) throw new HttpError(400, "File required");
    const stream = req.body.pipeThrough(
      new TransformStream<Uint8Array, Uint8Array>({
        transform(chunk, c) {
          received += chunk.length;
          if (received > MAX_UPLOAD || received > length)
            throw new Error("Upload size exceeded");
          c.enqueue(chunk);
        },
        flush() {
          if (received !== length) throw new Error("Upload length mismatch");
        },
      }),
    );
    const fixed = new FixedLengthStream(length);
    await Promise.all([
      stream.pipeTo(fixed.writable),
      env.FILES.put(`${member.tenant_id}/${id}/source.${ext}`, fixed.readable),
    ]);
    try {
      const inserted = await env.DB.prepare(
        "INSERT INTO designs(id,tenant_id,name,extension,units,status) SELECT ?,?,?,?,?,'queued_inspect' WHERE (SELECT COUNT(*) FROM designs WHERE tenant_id=? AND created_at >= datetime('now','-1 day')) < 20 RETURNING id",
      )
        .bind(id, member.tenant_id, name, ext, units, member.tenant_id)
        .first();
      if (!inserted) {
        await env.FILES.delete(`${member.tenant_id}/${id}/source.${ext}`);
        throw new HttpError(429, "Daily design limit reached");
      }
      await env.JOBS.send({ id, phase: "inspect" });
    } catch (error) {
      if (error instanceof HttpError) throw error;
      await env.DB.prepare(
        "UPDATE designs SET status='failed', error='Job enqueue failed. Retry inspection.' WHERE id=?",
      )
        .bind(id)
        .run();
      throw new HttpError(
        503,
        "Job enqueue failed; refresh the design list to retry",
      );
    }
    return Response.json({ id, status: "queued_inspect" }, { status: 202 });
  }
  const match = path.match(
    /^\/api\/designs\/(dg-[0-9a-f-]+)(?:\/(confirm|retry|files)(?:\/([a-zA-Z0-9_.-]+))?)?$/,
  );
  if (match) {
    const d = await owned(env, match[1], member);
    if (!match[2] && req.method === "GET")
      return Response.json({
        id: d.id,
        name: d.name,
        status:
          d.status === "ready" &&
          JSON.parse(d.inspection || "null")?.no_dot_contract !== 1
            ? "needs_review"
            : d.status,
        error: importError(d.error),
        inspection: await loadInspection(env, d),
      });
    if (match[2] === "confirm" && req.method === "POST") {
      const b = await body(req);
      if (b.confirmed !== true)
        throw new HttpError(400, "Confirm the converted preview and units");
      const inspection = await loadInspection(env, d);
      if (!inspection || inspection.no_dot_contract !== 1)
        throw new HttpError(409, "Inspect this drawing before generation");
      if (inspection.generation_supported === false)
        throw new HttpError(
          422,
          "Drawing imported; full-wafer generation is not yet available",
        );
      const selection = b.selected_regions ?? [];
      const count = inspection.region_candidates?.length ?? 0;
      if (
        !Array.isArray(selection) ||
        selection.length > 5000 ||
        selection.some((n) => !Number.isInteger(n) || n < 0 || n >= count)
      )
        throw new HttpError(400, "Invalid no-dot area selection");
      if (!(inspection.no_dot_regions > 0) && !selection.length)
        throw new HttpError(
          400,
          "Select the channel, chamber and port interiors before adding dots",
        );
      inspection.selected_regions = [...new Set(selection)];
      const changed = await env.DB.prepare(
        "UPDATE designs SET status='queued_generate',inspection=?,error=NULL,updated_at=CURRENT_TIMESTAMP WHERE id=? AND tenant_id=? AND status='awaiting_confirmation' RETURNING id",
      )
        .bind(
          inspectionRecord(d.inspection, inspection),
          d.id,
          member.tenant_id,
        )
        .first();
      if (!changed)
        throw new HttpError(409, "Design is not awaiting confirmation");
      try {
        await env.JOBS.send({ id: d.id, phase: "generate" });
      } catch {
        await env.DB.prepare(
          "UPDATE designs SET status='awaiting_confirmation' WHERE id=? AND status='queued_generate'",
        )
          .bind(d.id)
          .run();
        throw new HttpError(503, "Queue unavailable; confirm again");
      }
      return Response.json({ status: "queued_generate" }, { status: 202 });
    }
    if (match[2] === "retry" && req.method === "POST") {
      const changed = await env.DB.prepare(
        "UPDATE designs SET status='queued_inspect',error=NULL,inspection=NULL WHERE id=? AND status='failed' RETURNING id",
      )
        .bind(d.id)
        .first();
      if (!changed) throw new HttpError(409, "Only failed jobs can be retried");
      try {
        await env.JOBS.send({ id: d.id, phase: "inspect" });
      } catch {
        await env.DB.prepare(
          "UPDATE designs SET status='failed',error='Queue unavailable' WHERE id=?",
        )
          .bind(d.id)
          .run();
        throw new HttpError(503, "Queue unavailable");
      }
      return Response.json({ status: "queued_inspect" }, { status: 202 });
    }
    if (match[2] === "files" && req.method === "GET") {
      const filename = match[3];
      if (
        !CUSTOMER_FILES.some((f) => f === filename) ||
        d.status !== "ready" ||
        JSON.parse(d.inspection || "null")?.no_dot_contract !== 1
      )
        throw new HttpError(404, "File not available");
      const file = await env.FILES.get(
        `${member.tenant_id}/${d.id}/${filename}`,
      );
      if (!file) throw new HttpError(404, "File not available");
      return new Response(file.body, {
        headers: {
          "Content-Type": "application/octet-stream",
          "Content-Disposition": `attachment; filename="${filename}"`,
          "Cache-Control": "private, no-store",
        },
      });
    }
  }
  throw new HttpError(404, "Not found");
}
