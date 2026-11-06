import { it, expect } from "vitest";
import { spawn, execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { execute } from "../src/jobs";
import { db } from "./database";

it("runs real Python inspection and generation through job orchestration, publishes map only at success, replays safely", async () => {
  const folder = mkdtempSync(join(tmpdir(), "mib-portal-e2e-"));
  execFileSync(".venv/bin/python", [
    "-c",
    `import ezdxf\nd=ezdxf.new('R2010')\nd.header['$INSUNITS']=13\nd.modelspace().add_lwpolyline([(0,0),(6000,0),(6000,6000),(0,6000)],close=True)\nd.modelspace().add_line((1000,3000),(5000,3000))\nd.layers.new('NO_DOT')\nd.modelspace().add_lwpolyline([(1000,2900),(5000,2900),(5000,3100),(1000,3100)],close=True,dxfattribs={'layer':'NO_DOT'})\nd.saveas(${JSON.stringify(join(folder, "source.dxf"))})`,
  ]);
  const server = spawn(
    ".venv/bin/python",
    [
      "-m",
      "uvicorn",
      "generator.server:app",
      "--host",
      "127.0.0.1",
      "--port",
      "4180",
    ],
    {
      env: {
        ...process.env,
        CAD_WORKDIR: folder,
        OPENBLAS_NUM_THREADS: "1",
        OMP_NUM_THREADS: "1",
      },
      stdio: "ignore",
    },
  );
  const files = new Map<string, Uint8Array>();
  let calls = 0;
  let failArtifact = true;
  const env = {
    DB: db(),
    FILES: {
      async get(k: string) {
        const b = files.get(k);
        return b ? { body: new Blob([b]) } : null;
      },
      async put(k: string, b: any) {
        if (failArtifact && k.endsWith("dotgrid_layer.oas"))
          throw new Error("Injected R2 failure");
        files.set(k, new Uint8Array(await new Response(b).arrayBuffer()));
      },
    },
    CAD: {
      getByName() {
        return {
          async fetch(input: Request | string, init?: RequestInit) {
            calls++;
            const req =
              input instanceof Request ? input : new Request(input, init);
            return fetch(
              req.url.replace("http://cad", "http://127.0.0.1:4180"),
              {
                method: req.method,
                headers: req.headers,
                body:
                  req.method === "GET" ? undefined : await req.arrayBuffer(),
              },
            );
          },
          async stop() {},
        };
      },
    },
  } as any;
  try {
    const end = Date.now() + 10000;
    let healthy = false;
    while (Date.now() < end) {
      try {
        healthy = (await fetch("http://127.0.0.1:4180/health")).ok;
        if (healthy) break;
      } catch {}
      await new Promise((r) => setTimeout(r, 100));
    }
    expect(healthy).toBe(true);
    // Seed 8's rejection has a separate regression test; allocate known conforming fixture 9.
    await env.DB.prepare(
      "INSERT INTO designs(seed,id,name,extension,units,status) VALUES(8,'burned-fixture','Burned','','um','reserved')",
    ).run();
    await env.DB.prepare(
      "INSERT INTO designs(id,tenant_id,name,extension,units,status) VALUES('dg-aaaa','a','Fixture','dxf','um','queued_inspect')",
    ).run();
    files.set("a/dg-aaaa/source.dxf", readFileSync(join(folder, "source.dxf")));
    await execute({ id: "dg-aaaa", phase: "inspect" }, 1, env);
    expect(
      (
        await env.DB.prepare(
          "SELECT status FROM designs WHERE id='dg-aaaa'",
        ).first()
      ).status,
    ).toBe("awaiting_confirmation");
    expect(files.has("a/dg-aaaa/mib-map.zip")).toBe(false);
    const inspection = JSON.parse(
      (
        await env.DB.prepare(
          "SELECT inspection FROM designs WHERE id='dg-aaaa'",
        ).first()
      ).inspection,
    );
    inspection.manual_chip_rectangles = [[0, 0, 6000, 6000]];
    await env.DB.prepare(
      "UPDATE designs SET status='queued_generate',inspection=? WHERE id='dg-aaaa'",
    )
      .bind(JSON.stringify(inspection))
      .run();
    await expect(
      execute({ id: "dg-aaaa", phase: "generate" }, 1, env),
    ).rejects.toThrow("Injected R2 failure");
    expect(
      await env.DB.prepare(
        "SELECT status,registry_entry FROM designs WHERE id='dg-aaaa'",
      ).first(),
    ).toEqual({ status: "generating", registry_entry: null });
    failArtifact = false;
    await execute({ id: "dg-aaaa", phase: "generate" }, 2, env);
    const row = await env.DB.prepare(
      "SELECT status,registry_entry FROM designs WHERE id='dg-aaaa'",
    ).first();
    expect(row.status).toBe("ready");
    expect(JSON.parse(row.registry_entry).seed).toBe(9);
    for (const f of [
      "design_with_dotgrid.dxf",
      "dotgrid_layer.gds",
      "dotgrid_layer.oas",
      "mib-map.zip",
    ])
      expect(files.get(`a/dg-aaaa/${f}`)!.length).toBeGreaterThan(100);
    const before = calls;
    await execute({ id: "dg-aaaa", phase: "generate" }, 2, env);
    expect(calls).toBe(before);
  } finally {
    server.kill("SIGTERM");
    await new Promise<void>((r) => server.once("exit", () => r()));
  }
}, 30000);
