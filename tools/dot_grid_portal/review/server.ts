/** Local tailnet review adapter only. Never imported by the Cloudflare entrypoint. */
import { createServer } from "node:http";
import { DatabaseSync } from "node:sqlite";
import {
  mkdirSync,
  readFileSync,
  existsSync,
  createReadStream,
  createWriteStream,
} from "node:fs";
import {
  readFile,
  writeFile,
  unlink,
  mkdir,
  stat,
  rename,
} from "node:fs/promises";
import { resolve, dirname, extname } from "node:path";
import { Readable } from "node:stream";
import { pipeline } from "node:stream/promises";
import { api, type Job } from "../src/api";
import { execute } from "../src/jobs";
import { HttpError } from "../src/auth";
const NativeRequest = globalThis.Request;
(globalThis as any).Request = class extends NativeRequest {
  constructor(input: any, init?: any) {
    super(input, init?.body ? { ...init, duplex: "half" } : init);
  }
};
const origin = process.env.REVIEW_ORIGIN!;
if (!origin || !process.env.REVIEW_DATA)
  throw new Error("REVIEW_ORIGIN and REVIEW_DATA required");
const root = resolve(process.env.REVIEW_DATA);
mkdirSync(root, { recursive: true });
const fresh = !existsSync(`${root}/review.sqlite`);
const sql = new DatabaseSync(`${root}/review.sqlite`);
sql.exec("PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON;");
if (fresh) {
  sql.exec(readFileSync("migrations/0001_portal.sql", "utf8"));
  sql.exec(
    "INSERT INTO tenants VALUES('review','Tailnet review'); INSERT INTO members(email,tenant_id,role) VALUES('review@local.test','review','member'); INSERT INTO designs(seed,id,name,extension,units,status) VALUES(8,'review-reserved','Reserved review seed','','um','reserved');",
  );
}
const DB = {
  prepare(query: string) {
    let args: any[] = [];
    return {
      bind(...values: any[]) {
        args = values;
        return this;
      },
      async first() {
        return sql.prepare(query).get(...args) ?? null;
      },
      async all() {
        return { results: sql.prepare(query).all(...args) };
      },
      async run() {
        return sql.prepare(query).run(...args);
      },
    };
  },
};
(globalThis as any).FixedLengthStream = class extends TransformStream {
  constructor(length: number) {
    let count = 0;
    super({
      transform(chunk, controller) {
        count += chunk.length;
        if (count > length) throw new Error("Length exceeded");
        controller.enqueue(chunk);
      },
      flush() {
        if (count !== length) throw new Error("Length mismatch");
      },
    });
  }
};
function objectPath(k: string) {
  const p = resolve(root, "objects", k);
  if (!p.startsWith(`${root}/objects/`)) throw new Error("Invalid key");
  return p;
}
let queue = Promise.resolve();
const env: any = {
  DB,
  FILES: {
    async get(k: string) {
      try {
        await stat(objectPath(k));
        return { body: Readable.toWeb(createReadStream(objectPath(k))) };
      } catch (e: any) {
        if (e.code === "ENOENT") return null;
        throw e;
      }
    },
    async put(k: string, body: any) {
      const p = objectPath(k);
      await mkdir(dirname(p), { recursive: true });
      const temporary = `${p}.part`;
      try {
        const stream = new Response(body).body;
        if (!stream) throw new Error("Missing object body");
        await pipeline(
          Readable.fromWeb(stream as any),
          createWriteStream(temporary),
        );
        await rename(temporary, p);
      } catch (error) {
        await unlink(temporary).catch(() => {});
        throw error;
      }
    },
    async delete(k: string) {
      await unlink(objectPath(k));
    },
  },
  CAD: {
    getByName() {
      return {
        async fetch(input: Request | string, init?: RequestInit) {
          const r = input instanceof Request ? input : new Request(input, init);
          return fetch(r.url.replace("http://cad", "http://127.0.0.1:19838"), {
            method: r.method,
            headers: r.headers,
            body: r.method === "GET" ? undefined : await r.arrayBuffer(),
            signal: AbortSignal.timeout(310000),
          });
        },
        async stop() {},
      };
    },
  },
  JOBS: {
    async send(job: Job) {
      queue = queue
        .then(async () => {
          try {
            await execute(job, 1, env);
          } catch (e: any) {
            await DB.prepare(
              "UPDATE designs SET status='failed',error=? WHERE id=? AND status NOT IN ('ready','awaiting_confirmation')",
            )
              .bind(String(e.message).slice(0, 1000), job.id)
              .run();
          }
        })
        .catch((e) => console.error(e));
    },
  },
};
// Resume persisted interrupted jobs in order after service restart.
for (const row of sql
  .prepare(
    "SELECT id,status FROM designs WHERE status IN ('queued_inspect','inspecting','queued_generate','generating') ORDER BY seed",
  )
  .all()) {
  await env.JOBS.send({
    id: row.id,
    phase: String(row.status).includes("inspect") ? "inspect" : "generate",
  });
}
const member = {
  email: "review@local.test",
  tenant_id: "review",
  role: "member" as const,
  active: 1,
};
createServer(async (req, res) => {
  try {
    const url = new URL(req.url!, origin);
    if (url.origin !== origin) throw new HttpError(400, "Invalid target");
    const headers = new Headers();
    for (const [k, v] of Object.entries(req.headers))
      if (v) headers.set(k, Array.isArray(v) ? v.join(",") : v);
    const request = new Request(url, {
      method: req.method,
      headers,
      body: ["GET", "HEAD"].includes(req.method!)
        ? undefined
        : (Readable.toWeb(req) as any),
      duplex: "half",
    } as any);
    let response: Response;
    if (url.pathname.startsWith("/api/"))
      response = await api(request, env, member);
    else {
      const path = resolve(
        "dist",
        url.pathname === "/"
          ? "index.html"
          : "." + decodeURIComponent(url.pathname),
      );
      if (!path.startsWith(resolve("dist") + "/"))
        throw new HttpError(404, "Not found");
      let data = await readFile(path);
      if (path.endsWith("index.html"))
        data = Buffer.from(
          data
            .toString()
            .replace(
              "<body>",
              '<body><aside style="padding:12px;background:#fff0c2;color:#402b00;text-align:center">Tailnet review · shared workspace · real CAD processing · not production login. Proprietary dot technology.</aside>',
            ),
        );
      response = new Response(data, {
        headers: {
          "Content-Type":
            (
              {
                ".html": "text/html",
                ".js": "text/javascript",
                ".css": "text/css",
                ".svg": "image/svg+xml",
              } as any
            )[extname(path)] || "application/octet-stream",
        },
      });
    }
    res.writeHead(response.status, {
      ...Object.fromEntries(response.headers),
      "Cache-Control": "no-store",
      "X-Content-Type-Options": "nosniff",
      "Content-Security-Policy":
        "default-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; frame-ancestors 'none'; base-uri 'none'",
    });
    if (response.body) Readable.fromWeb(response.body as any).pipe(res);
    else res.end();
  } catch (e: any) {
    res.writeHead(
      e instanceof HttpError ? e.status : e.code === "ENOENT" ? 404 : 500,
      { "Content-Type": "application/json" },
    );
    res.end(JSON.stringify({ error: e.message }));
  }
}).listen(19837, "127.0.0.1", () =>
  console.log("Review listening on loopback:19837"),
);
