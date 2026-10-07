// @vitest-environment jsdom
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { isString, isStringArray, readPersisted, usePersistedState, writePersisted } from "./persistedState";

let host: HTMLDivElement, root: Root;
let setter: (v: string | ((p: string) => string)) => void;
let current = "";
function Probe({ k }: { k: string }) {
  const [v, set] = usePersistedState(k, "none", isString);
  setter = set; current = v;
  return <span>{v}</span>;
}

beforeEach(() => {
  Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
  window.sessionStorage.clear();
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); vi.restoreAllMocks(); });

describe("state that survives a reload (#550 m14)", () => {
  it("restores what an earlier mount stored, and falls back when nothing valid is there", async () => {
    await act(async () => root.render(<Probe k="t1" />));
    expect(current).toBe("none");
    await act(async () => setter("device|core:1"));
    expect(window.sessionStorage.getItem("t1")).toBe('"device|core:1"');
    await act(async () => root.unmount());
    root = createRoot(host);
    await act(async () => root.render(<Probe k="t1" />));
    expect(current).toBe("device|core:1");
    expect(readPersisted("t2", "x", isString)).toBe("x");
    window.sessionStorage.setItem("t3", "{broken");
    expect(readPersisted("t3", "x", isString)).toBe("x");
    window.sessionStorage.setItem("t4", "42");
    expect(readPersisted("t4", "x", isString)).toBe("x");
  });

  it("supports functional updates", async () => {
    await act(async () => root.render(<Probe k="t5" />));
    await act(async () => setter((p) => `${p}+1`));
    expect(current).toBe("none+1");
  });

  it("works without storage", async () => {
    vi.spyOn(Storage.prototype, "getItem").mockImplementation(() => { throw new Error("blocked"); });
    vi.spyOn(Storage.prototype, "setItem").mockImplementation(() => { throw new Error("blocked"); });
    await act(async () => root.render(<Probe k="t6" />));
    await act(async () => setter("kept in memory"));
    expect(current).toBe("kept in memory");
    expect(() => writePersisted("t7", 1)).not.toThrow();
  });

  it("validates a stored log", () => {
    expect(isStringArray(["a", "b"])).toBe(true);
    expect(isStringArray(["a", 1])).toBe(false);
    expect(isStringArray("a")).toBe(false);
  });
});
