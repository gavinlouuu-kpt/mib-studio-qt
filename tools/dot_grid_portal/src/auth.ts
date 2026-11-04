import { createRemoteJWKSet, jwtVerify, type JWTVerifyGetKey } from "jose";
export type Member = {
  email: string;
  tenant_id: string;
  role: "admin" | "member";
  active: number;
};
export class HttpError extends Error {
  constructor(
    public status: number,
    message: string,
  ) {
    super(message);
  }
}
export async function verifyIdentity(
  token: string,
  issuer: string,
  audience: string,
  resolver?: JWTVerifyGetKey,
): Promise<string> {
  if (
    !/^https:\/\/[a-z0-9-]+\.cloudflareaccess\.com$/.test(issuer) ||
    !audience ||
    audience.startsWith("configure-")
  )
    throw new HttpError(503, "Access is not configured");
  try {
    const { payload } = await jwtVerify(
      token,
      resolver ?? createRemoteJWKSet(new URL(`${issuer}/cdn-cgi/access/certs`)),
      {
        issuer,
        audience,
        algorithms: ["RS256"],
        requiredClaims: ["exp", "iat", "sub", "email"],
      },
    );
    if (typeof payload.email !== "string" || !payload.email.includes("@"))
      throw new Error("Invalid email");
    return payload.email.toLowerCase();
  } catch {
    throw new HttpError(401, "Sign in through the portal Access login");
  }
}
export async function authenticate(req: Request, env: Env): Promise<Member> {
  const token = req.headers.get("Cf-Access-Jwt-Assertion");
  if (!token)
    throw new HttpError(401, "Sign in through the portal Access login");
  const email = await verifyIdentity(token, env.ACCESS_ISSUER, env.ACCESS_AUD);
  const member = await env.DB.prepare(
    "SELECT * FROM members WHERE email=? AND active=1",
  )
    .bind(email)
    .first<Member>();
  if (!member) throw new HttpError(403, "An active invitation is required");
  return member;
}
export function checkMutation(req: Request) {
  if (
    !["GET", "HEAD"].includes(req.method) &&
    req.headers.get("Origin") !== new URL(req.url).origin
  )
    throw new HttpError(403, "Same-origin request required");
}
