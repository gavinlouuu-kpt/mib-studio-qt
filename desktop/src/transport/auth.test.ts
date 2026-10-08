// @vitest-environment jsdom
// #501: token resolution and the /auth probe the browser UI runs before opening the socket.
import { afterEach, describe, expect, it, vi } from "vitest";
import { authUrl, probeAuth, storeToken, tokenFromLocation } from "./auth";
import { wsUrlFromLocation } from "./wsTransport";

const at = (href: string) => new URL(href) as unknown as Location;

afterEach(() => window.sessionStorage.clear());

describe("auth", () => {
  it("builds /auth on the page origin or on ?server's host", () => {
    expect(authUrl(at("http://board:8427/"), "")).toBe("http://board:8427/auth");
    expect(authUrl(at("http://board:8427/?token=a b"))).toBe("http://board:8427/auth?token=a%20b");
    expect(authUrl(at("http://localhost:1420/?server=wss://board/ws"), "t")).toBe("https://board/auth?token=t");
  });

  it("a typed token is used when the URL has none; the URL wins", () => {
    storeToken("typed");
    expect(tokenFromLocation(at("http://board/"))).toBe("typed");
    expect(tokenFromLocation(at("http://board/?token=url"))).toBe("url");
    expect(wsUrlFromLocation(at("http://board:8427/"))).toBe("ws://board:8427/ws?token=typed");
  });

  it("maps the probe: 401 prompts, a network error is unreachable, older servers pass", async () => {
    const reply = (status: number) => vi.fn().mockResolvedValue({ status } as Response);
    expect(await probeAuth("u", reply(401) as unknown as typeof fetch)).toBe("unauthorized");
    expect(await probeAuth("u", reply(200) as unknown as typeof fetch)).toBe("authorized");
    expect(await probeAuth("u", reply(404) as unknown as typeof fetch)).toBe("authorized");
    expect(await probeAuth("u", vi.fn().mockRejectedValue(new TypeError("down")) as unknown as typeof fetch)).toBe("unreachable");
  });
});
