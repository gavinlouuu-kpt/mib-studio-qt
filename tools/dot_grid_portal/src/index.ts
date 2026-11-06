import { Container } from "@cloudflare/containers";
import { authenticate, HttpError } from "./auth";
import { api, type Job } from "./api";
import { execute } from "./jobs";
export class CadContainer extends Container<Env> {
  defaultPort = 8080;
  sleepAfter = "10m";
  enableInternet = false;
}
const jsonHeaders = {
  "Cache-Control": "private, no-store",
  "X-Content-Type-Options": "nosniff",
  "Referrer-Policy": "same-origin",
  "Content-Security-Policy":
    "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'",
};
export default {
  async fetch(req: Request, env: Env): Promise<Response> {
    try {
      const member = await authenticate(req, env);
      const r = new URL(req.url).pathname.startsWith("/api/")
        ? await api(req, env, member)
        : await env.ASSETS.fetch(req);
      const out = new Response(r.body, r);
      for (const [k, v] of Object.entries(jsonHeaders)) out.headers.set(k, v);
      return out;
    } catch (error) {
      const expected = error instanceof HttpError;
      if (!expected)
        console.error(
          JSON.stringify({
            event: "request_failed",
            type: error instanceof Error ? error.name : "unknown",
          }),
        );
      return Response.json(
        { error: expected ? error.message : "Request failed" },
        { status: expected ? error.status : 500, headers: jsonHeaders },
      );
    }
  },
  async queue(batch: MessageBatch<Job>, env: Env): Promise<void> {
    for (const m of batch.messages) {
      try {
        await execute(m.body, m.attempts, env);
        m.ack();
      } catch (e) {
        const error = e instanceof Error ? e.message : "CAD processing failed";
        if (m.attempts >= 3) {
          await env.DB.prepare(
            "UPDATE designs SET status='failed',error=?,updated_at=CURRENT_TIMESTAMP WHERE id=? AND status NOT IN ('ready','awaiting_confirmation')",
          )
            .bind(error.slice(0, 1000), m.body.id)
            .run();
          m.ack();
        } else m.retry({ delaySeconds: 30 });
      }
    }
  },
} satisfies ExportedHandler<Env, Job>;
