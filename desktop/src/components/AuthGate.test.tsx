// @vitest-environment jsdom
// #501: in the browser the app mounts only once the token is accepted; otherwise a prompt.
import { act, useEffect } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

vi.mock("../transport", () => ({ isRemote: true, transport: { kind: "ws", invoke: vi.fn() } }));
import { AuthGate } from "./AuthGate";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
let root: Root, host: HTMLDivElement;
const fetchMock = vi.fn();

beforeEach(() => {
  window.sessionStorage.clear();
  fetchMock.mockReset();
  vi.stubGlobal("fetch", fetchMock);
  host = document.createElement("div");
  document.body.appendChild(host);
  root = createRoot(host);
});
afterEach(async () => {
  await act(async () => root.unmount());
  host.remove();
  vi.unstubAllGlobals();
});

const render = async () => act(async () => root.render(<AuthGate><p>app</p></AuthGate>));

it("prompts for the token, keeps the app unmounted, then mounts it once accepted", async () => {
  fetchMock.mockResolvedValueOnce({ status: 401 });
  await render();
  expect(host.textContent).toContain("Access token");
  expect(host.textContent).not.toContain("app");

  const input = host.querySelector("input")!;
  await act(async () => {
    const setter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!;
    setter.call(input, "secret");
    input.dispatchEvent(new Event("input", { bubbles: true }));
  });
  fetchMock.mockResolvedValueOnce({ status: 200 });
  await act(async () => host.querySelector("form")!.dispatchEvent(new Event("submit", { bubbles: true, cancelable: true })));
  expect(fetchMock.mock.calls[1][0]).toContain("/auth?token=secret");
  expect(host.textContent).toContain("app");
  expect(window.sessionStorage.getItem("yofo-studio-token")).toBe("secret");
});

it("a wrong token says so", async () => {
  fetchMock.mockResolvedValue({ status: 401 });
  await render();
  const input = host.querySelector("input")!;
  await act(async () => {
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!.call(input, "wrong");
    input.dispatchEvent(new Event("input", { bubbles: true }));
  });
  await act(async () => host.querySelector("form")!.dispatchEvent(new Event("submit", { bubbles: true, cancelable: true })));
  expect(host.textContent).toContain("not accepted");
});

it("an unreachable server offers a retry", async () => {
  fetchMock.mockRejectedValue(new TypeError("down"));
  await render();
  expect(host.textContent).toContain("unreachable");
});

// #501: the link drops or the board reboots while the page is open: no page reload.
const reply = (status: number, boot?: string) => ({ status, json: async () => (boot ? { boot_id: boot } : {}) });
let mounts = 0;
function Probe() {
  useEffect(() => { mounts += 1; }, []);
  return <p>app</p>;
}
const renderProbe = async () => act(async () => root.render(<AuthGate monitorMs={1000}><Probe /></AuthGate>));
const tick = async (ms = 1000) => act(async () => { await vi.advanceTimersByTimeAsync(ms); });

describe("reconnecting while the page is open", () => {
  beforeEach(() => { mounts = 0; vi.useFakeTimers(); });
  afterEach(() => { vi.useRealTimers(); });

  it("shows a banner while the server is unreachable and carries on when it answers with the same boot id", async () => {
    fetchMock.mockResolvedValue(reply(200, "boot-a"));
    await renderProbe();
    expect(host.textContent).toContain("app");
    fetchMock.mockRejectedValue(new TypeError("down"));
    await tick();
    expect(host.textContent).not.toContain("lost"); // one miss is a blip
    await tick();
    expect(host.textContent).toContain("Connection to the instrument lost");
    expect(host.textContent).toContain("app");
    fetchMock.mockResolvedValue(reply(200, "boot-a"));
    await tick();
    expect(host.textContent).not.toContain("lost");
    expect(mounts).toBe(1); // the app was never re-mounted
  });

  it("re-mounts the app when the backend restarted (a new boot id), and says so", async () => {
    fetchMock.mockResolvedValue(reply(200, "boot-a"));
    await renderProbe();
    fetchMock.mockRejectedValue(new TypeError("down"));
    await tick(2000);
    fetchMock.mockResolvedValue(reply(200, "boot-b"));
    await tick();
    expect(mounts).toBe(2);
    expect(host.textContent).toContain("The instrument restarted");
    await act(async () => host.querySelector("button")!.click());
    expect(host.textContent).not.toContain("The instrument restarted");
    expect(host.textContent).toContain("app");
  });

  it("asks for the token again over the running app when the new backend rejects it, then carries on", async () => {
    fetchMock.mockResolvedValue(reply(200, "boot-a"));
    await renderProbe();
    fetchMock.mockResolvedValue(reply(401, "boot-b"));
    await tick();
    expect(host.textContent).toContain("Access token");
    expect(host.textContent).toContain("app"); // still mounted underneath
    const input = host.querySelector("input")!;
    await act(async () => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!.call(input, "new-token");
      input.dispatchEvent(new Event("input", { bubbles: true }));
    });
    fetchMock.mockResolvedValue(reply(200, "boot-b"));
    await act(async () => host.querySelector("form")!.dispatchEvent(new Event("submit", { bubbles: true, cancelable: true })));
    expect(fetchMock.mock.calls[fetchMock.mock.calls.length - 1][0]).toContain("/auth?token=new-token");
    expect(host.textContent).not.toContain("Access token");
    expect(window.sessionStorage.getItem("yofo-studio-token")).toBe("new-token");
  });

  it("an older server without a boot id never triggers a re-mount", async () => {
    fetchMock.mockResolvedValue({ status: 200 });
    await renderProbe();
    await tick(3000);
    expect(mounts).toBe(1);
  });
});
