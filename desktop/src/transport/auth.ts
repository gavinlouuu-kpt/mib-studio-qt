// #501: the instrument's server wants a token. A browser cannot read why a WebSocket upgrade
// failed, so the UI asks GET /auth first and prompts for the token instead of showing
// "backend: not initialized". The token comes from ?token= or, once typed, sessionStorage
// (this tab only; cleared when it closes). Never logged.

const TOKEN_KEY = "yofo-studio-token";

export function storedToken(): string {
  try {
    return window.sessionStorage.getItem(TOKEN_KEY) ?? "";
  } catch {
    return "";
  }
}

export function storeToken(token: string): void {
  try {
    window.sessionStorage.setItem(TOKEN_KEY, token);
  } catch {
    /* storage unavailable: the token lives for this page load only */
  }
  memoryToken = token;
}

let memoryToken = "";

/** ?token= wins, then the token typed into the prompt. */
export function tokenFromLocation(location: Location = window.location): string {
  return new URLSearchParams(location.search).get("token") ?? (storedToken() || memoryToken);
}

/** The server's /auth URL: the page origin, or the origin of ?server=ws(s)://host/ws. */
export function authUrl(location: Location = window.location, token = tokenFromLocation(location)): string {
  const explicit = new URLSearchParams(location.search).get("server");
  let origin = location.origin;
  if (explicit) {
    const u = new URL(explicit);
    origin = `${u.protocol === "wss:" ? "https:" : "http:"}//${u.host}`;
  }
  return `${origin}/auth${token ? `?token=${encodeURIComponent(token)}` : ""}`;
}

export type AuthOutcome = "authorized" | "unauthorized" | "unreachable";

export async function probeAuth(url: string, fetchImpl: typeof fetch = fetch): Promise<AuthOutcome> {
  try {
    const response = await fetchImpl(url, { cache: "no-store" });
    if (response.status === 401) return "unauthorized";
    // Older servers have no /auth: let the socket decide.
    return "authorized";
  } catch {
    return "unreachable";
  }
}
