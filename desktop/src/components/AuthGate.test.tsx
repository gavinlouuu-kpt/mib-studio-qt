// @vitest-environment jsdom
// #501: in the browser the app mounts only once the token is accepted; otherwise a prompt.
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, expect, it, vi } from "vitest";

vi.mock("../transport", () => ({ isRemote: true }));
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
