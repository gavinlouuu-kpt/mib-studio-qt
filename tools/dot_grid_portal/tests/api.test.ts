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
    FILES: { get: async () => ({ body: "secret" }) },
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
