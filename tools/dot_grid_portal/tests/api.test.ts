import { describe, it, expect, beforeEach } from "vitest";
import { db } from "./database";
import { api, owned } from "../src/api";
import { checkMutation, verifyIdentity, type Member } from "../src/auth";
import { generateKeyPair, SignJWT, createLocalJWKSet, exportJWK } from "jose";

// Real SQLite constraints and queries; adapter matches the D1 methods used here.
let env: Env;
const admin: Member = {
  email: "admin@a.test",
  tenant_id: "a",
  role: "admin",
  active: 1,
};
const other: Member = {
  email: "member@b.test",
  tenant_id: "b",
  role: "member",
  active: 1,
};
const req = (path: string, method = "GET", data?: unknown) =>
  new Request(`https://portal.test/api${path}`, {
    method,
    headers: {
      Origin: "https://portal.test",
      "Content-Type": "application/json",
    },
    body: data ? JSON.stringify(data) : undefined,
  });
beforeEach(() => {
  env = {
    DB: db(),
    FILES: {
      get: async (key: string) =>
        key.endsWith("dot-preview.json") ? null : { body: "secret" },
    },
    JOBS: { send: async () => {} },
  } as any;
});
describe("authorization and registration boundaries", () => {
  it("does not expose other tenants designs or files", async () => {
    await env.DB.prepare(
      "INSERT INTO designs(id,tenant_id,name,extension,units,status) VALUES('dg-aaaa','a','Secret','dxf','um','ready')",
    ).run();
    expect(await (await api(req("/designs"), env, other)).json()).toEqual([]);
    await expect(
      api(req("/designs/dg-aaaa"), env, other),
    ).rejects.toMatchObject({ status: 404 });
    await expect(
      api(req("/designs/dg-aaaa/files/mib-map.zip"), env, other),
    ).rejects.toMatchObject({ status: 404 });
  });
  it("blocks member invites and cross-tenant reassignment", async () => {
    await expect(
      api(req("/members", "POST", { email: "x@test.org" }), env, other),
    ).rejects.toMatchObject({ status: 403 });
    await expect(
      api(req("/members", "POST", { email: other.email }), env, admin),
    ).rejects.toMatchObject({ status: 409 });
    expect(
      await env.DB.prepare("SELECT tenant_id FROM members WHERE email=?")
        .bind(other.email)
        .first(),
    ).toEqual({ tenant_id: "b" });
  });
  it("creates member invitation without privilege escalation and revokes only own members", async () => {
    expect(
      (
        await api(
          req("/members", "POST", {
            email: "New@Test.org",
            role: "admin",
            tenant_id: "b",
          }),
          env,
          admin,
        )
      ).status,
    ).toBe(201);
    expect(
      await env.DB.prepare(
        "SELECT tenant_id,role,active FROM members WHERE email=?",
      )
        .bind("new@test.org")
        .first(),
    ).toEqual({ tenant_id: "a", role: "member", active: 1 });
    await expect(
      api(req("/members", "PATCH", { email: other.email }), env, admin),
    ).rejects.toMatchObject({ status: 404 });
    await api(req("/members", "PATCH", { email: "new@test.org" }), env, admin);
    expect(
      await env.DB.prepare("SELECT active FROM members WHERE email=?")
        .bind("new@test.org")
        .first(),
    ).toEqual({ active: 0 });
  });
  it("requires confirmation, prevents duplicate generation, reserves seeds monotonically", async () => {
    for (const id of ["dg-aaaa", "dg-bbbb"])
      await env.DB.prepare(
        "INSERT INTO designs(id,tenant_id,name,extension,units,status) VALUES(?,'a','Test','dxf','um','awaiting_confirmation')",
      )
        .bind(id)
        .run();
    await env.DB.prepare("UPDATE designs SET inspection=?")
      .bind(JSON.stringify({ no_dot_contract: 1, no_dot_regions: 1 }))
      .run();
    expect((await owned(env, "dg-aaaa", admin)).seed).toBe(8);
    expect((await owned(env, "dg-bbbb", admin)).seed).toBe(9);
    await expect(
      api(
        req("/designs/dg-aaaa/confirm", "POST", { confirmed: false }),
        env,
        admin,
      ),
    ).rejects.toMatchObject({ status: 400 });
    await api(
      req("/designs/dg-aaaa/confirm", "POST", { confirmed: true }),
      env,
      admin,
    );
    await expect(
      api(
        req("/designs/dg-aaaa/confirm", "POST", { confirmed: true }),
        env,
        admin,
      ),
    ).rejects.toMatchObject({ status: 409 });
    await expect(
      api(req("/designs/dg-aaaa/files/mib-map.zip"), env, admin),
    ).rejects.toMatchObject({ status: 404 });
  });
  it("restores confirmation state when queue fails", async () => {
    await env.DB.prepare(
      "INSERT INTO designs(id,tenant_id,name,extension,units,status) VALUES('dg-aaaa','a','Test','dxf','um','awaiting_confirmation')",
    ).run();
    await env.DB.prepare("UPDATE designs SET inspection=?")
      .bind(JSON.stringify({ no_dot_contract: 1, no_dot_regions: 1 }))
      .run();
    env.JOBS.send = async () => {
      throw new Error("offline");
    };
    await expect(
      api(
        req("/designs/dg-aaaa/confirm", "POST", { confirmed: true }),
        env,
        admin,
      ),
    ).rejects.toMatchObject({ status: 503 });
    expect((await owned(env, "dg-aaaa", admin)).status).toBe(
      "awaiting_confirmation",
    );
  });
  it("blocks cross-origin mutation", () => {
    expect(() =>
      checkMutation(
        new Request("https://portal.test/api/designs", {
          method: "POST",
          headers: { Origin: "https://evil.test" },
        }),
      ),
    ).toThrow("Same-origin");
  });
});
describe("Access JWT cryptography", () => {
  it("accepts signed correct identity and rejects wrong audience, expired and forged tokens", async () => {
    const { privateKey, publicKey } = await generateKeyPair("RS256");
    const key = { ...(await exportJWK(publicKey)), kid: "test", alg: "RS256" };
    const jwks = createLocalJWKSet({ keys: [key] });
    const sign = (aud = "portal", exp = "1h") =>
      new SignJWT({ email: "Member@A.test" })
        .setProtectedHeader({ alg: "RS256", kid: "test" })
        .setSubject("subject")
        .setIssuer("https://team.cloudflareaccess.com")
        .setAudience(aud)
        .setIssuedAt()
        .setExpirationTime(exp)
        .sign(privateKey);
    expect(
      await verifyIdentity(
        await sign(),
        "https://team.cloudflareaccess.com",
        "portal",
        jwks,
      ),
    ).toBe("member@a.test");
    await expect(
      verifyIdentity(
        await sign("other"),
        "https://team.cloudflareaccess.com",
        "portal",
        jwks,
      ),
    ).rejects.toMatchObject({ status: 401 });
    await expect(
      verifyIdentity(
        await sign("portal", "-1h"),
        "https://team.cloudflareaccess.com",
        "portal",
        jwks,
      ),
    ).rejects.toMatchObject({ status: 401 });
    await expect(
      verifyIdentity(
        "forged",
        "https://team.cloudflareaccess.com",
        "portal",
        jwks,
      ),
    ).rejects.toMatchObject({ status: 401 });
  });
});

it("never exposes proprietary maps or seeds and blocks unverified legacy exports", async () => {
  await env.DB.prepare(
    "INSERT INTO designs(id,tenant_id,name,extension,units,status,inspection,registry_entry) VALUES('dg-cccc','a','Device','dxf','um','ready',?,?)",
  )
    .bind(JSON.stringify({ no_dot_contract: 1 }), JSON.stringify({ seed: 123 }))
    .run();
  const detail = await (await api(req("/designs/dg-cccc"), env, admin)).json();
  expect(detail).not.toHaveProperty("seed");
  expect(detail).not.toHaveProperty("registry_entry");
  for (const role of ["admin", "member"] as const) {
    await expect(
      api(req("/designs/dg-cccc/files/mib-map.zip"), env, { ...admin, role }),
    ).rejects.toMatchObject({ status: 404 });
    expect(
      (
        await api(req("/designs/dg-cccc/files/design_with_dotgrid.dxf"), env, {
          ...admin,
          role,
        })
      ).status,
    ).toBe(200);
  }
  await env.DB.prepare(
    "UPDATE designs SET inspection=NULL WHERE id='dg-cccc'",
  ).run();
  await expect(
    api(req("/designs/dg-cccc/files/design_with_dotgrid.dxf"), env, admin),
  ).rejects.toMatchObject({ status: 404 });
  expect(
    (await (await api(req("/designs/dg-cccc"), env, admin)).json()).status,
  ).toBe("needs_review");
});

it("validates no-dot selections and capacity server-side", async () => {
  const i = {
    no_dot_contract: 1,
    no_dot_regions: 0,
    region_candidates: [{ id: 0 }],
    generation_supported: true,
  };
  await env.DB.prepare(
    "INSERT INTO designs(id,tenant_id,name,extension,units,status,inspection) VALUES('dg-cccc','a','Plain','dxf','um','awaiting_confirmation',?)",
  )
    .bind(JSON.stringify(i))
    .run();
  for (const selection of [[], [-1], [1], [0.5], ["0"]]) {
    await expect(
      api(
        req("/designs/dg-cccc/confirm", "POST", {
          confirmed: true,
          selected_regions: selection,
        }),
        env,
        admin,
      ),
    ).rejects.toMatchObject({ status: 400 });
  }
  await env.DB.prepare("UPDATE designs SET inspection=? WHERE id='dg-cccc'")
    .bind(JSON.stringify({ ...i, generation_supported: false }))
    .run();
  await expect(
    api(
      req("/designs/dg-cccc/confirm", "POST", {
        confirmed: true,
        selected_regions: [0],
      }),
      env,
      admin,
    ),
  ).rejects.toMatchObject({ status: 422 });
  await env.DB.prepare("UPDATE designs SET inspection=? WHERE id='dg-cccc'")
    .bind(JSON.stringify(i))
    .run();
  await api(
    req("/designs/dg-cccc/confirm", "POST", {
      confirmed: true,
      selected_regions: [0],
    }),
    env,
    admin,
  );
  expect(
    JSON.parse((await owned(env, "dg-cccc", admin)).inspection!)
      .selected_regions,
  ).toEqual([0]);
});

it("keeps large geometry in private storage and persists only validated choices in SQL", async () => {
  const geometry = {
    no_dot_contract: 1,
    no_dot_regions: 0,
    generation_supported: true,
    region_candidates: [
      {
        id: 0,
        kind: "polygon",
        points: [
          [0, 0],
          [1, 0],
          [0, 1],
        ],
      },
    ],
  };
  const requested: string[] = [];
  env.FILES.get = async (key: string) => {
    requested.push(key);
    return { body: JSON.stringify(geometry) } as any;
  };
  await env.DB.prepare(
    "INSERT INTO designs(id,tenant_id,name,extension,units,status,inspection) VALUES('dg-dddd','a','Large','dxf','um','awaiting_confirmation',?)",
  )
    .bind(JSON.stringify({ geometry_object: true, no_dot_contract: 1 }))
    .run();
  const d: any = await (await api(req("/designs/dg-dddd"), env, admin)).json();
  expect(d.inspection.region_candidates).toEqual(geometry.region_candidates);
  await expect(api(req("/designs/dg-dddd"), env, other)).rejects.toMatchObject({
    status: 404,
  });
  await api(
    req("/designs/dg-dddd/confirm", "POST", {
      confirmed: true,
      selected_regions: [0],
    }),
    env,
    admin,
  );
  const stored = JSON.parse((await owned(env, "dg-dddd", admin)).inspection!);
  expect(stored).toEqual({
    geometry_object: true,
    no_dot_contract: 1,
    selected_regions: [0],
  });
  expect(requested).toEqual([
    "a/dg-dddd/inspection.json",
    "a/dg-dddd/inspection.json",
  ]);
});

it("exposes actionable import diagnostics without leaking worker internals", async () => {
  await env.DB.prepare(
    "INSERT INTO designs(id,tenant_id,name,extension,units,status,error) VALUES('dg-eeee','a','Failed','dwg','um','failed',?)",
  )
    .bind("CAD_GEOMETRY_LOSS: /private/source.dwg seed=99")
    .run();
  const response: any = await (
    await api(req("/designs/dg-eeee"), env, admin)
  ).json();
  expect(response.error).toContain("original drawing is retained");
  expect(JSON.stringify(response)).not.toContain("/private");
  expect(JSON.stringify(response)).not.toContain("seed");
});
