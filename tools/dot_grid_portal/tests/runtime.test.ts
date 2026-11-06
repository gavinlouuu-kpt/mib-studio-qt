import { it, expect } from "vitest";
import { build } from "esbuild";
import { Miniflare, convertV4MiniflareOptions } from "miniflare";
import { readFileSync } from "node:fs";
it("streams a bounded upload into actual local R2 and reserves a D1 identity", async () => {
  const result = await build({
    stdin: {
      contents: `import {api} from './src/api'; export default {fetch(req,env){return api(req,{...env,JOBS:{send:async()=>{}}},{email:'admin@a.test',tenant_id:'a',role:'admin',active:1}).catch(e=>Response.json({error:e.message},{status:e.status||500}));}}`,
      resolveDir: process.cwd(),
      sourcefile: "runtime-harness.ts",
    },
    bundle: true,
    write: false,
    format: "esm",
    platform: "neutral",
    external: ["node:*"],
    target: "es2022",
  });
  const mf = new Miniflare(
    convertV4MiniflareOptions({
      workers: [
        {
          name: "portal-test",
          modules: true,
          script: result.outputFiles[0].text,
          compatibilityDate: "2026-11-04",
          compatibilityFlags: ["nodejs_compat"],
          d1Databases: ["DB"],
          r2Buckets: ["FILES"],
        },
      ],
    }),
  );
  try {
    const db = await mf.getD1Database("DB");
    for (const statement of readFileSync("migrations/0001_portal.sql", "utf8")
      .replace(/--[^\n]*/g, "")
      .split(";")
      .filter((x) => x.trim()))
      await db.prepare(statement).run();
    await db.prepare("INSERT INTO tenants VALUES('a','A')").run();
    const request = () =>
      mf.dispatchFetch(
        "http://localhost/api/designs?name=Test&extension=dxf&units=um",
        {
          method: "POST",
          headers: {
            Origin: "http://localhost",
            "Content-Type": "application/octet-stream",
          },
          body: "real uploaded bytes",
        },
      );
    const res = await request();
    expect(res.status).toBe(202);
    const data = (await res.json()) as { id: string };
    const file = await (
      await mf.getR2Bucket("FILES")
    ).get(`a/${data.id}/source.dxf`);
    expect(await file!.text()).toBe("real uploaded bytes");
    const row = await db
      .prepare("SELECT seed,status FROM designs WHERE id=?")
      .bind(data.id)
      .first();
    expect(row).toEqual({ seed: 8, status: "queued_inspect" });
  } finally {
    await mf.dispose();
  }
}, 20000);
