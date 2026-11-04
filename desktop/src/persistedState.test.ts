// @vitest-environment jsdom
import { beforeEach, describe, expect, it, vi } from "vitest";
import { isString, readPersisted, writePersisted } from "./persistedState";

// A plain in-memory store per kind, as the other tests do: the Node versions the suite runs on differ in what jsdom's own storage does.
function memoryStorage() {
  const saved = new Map<string, string>();
  return {
    getItem: vi.fn((key: string) => saved.get(key) ?? null),
    setItem: vi.fn((key: string, value: string) => void saved.set(key, value)),
  };
}
let local: ReturnType<typeof memoryStorage>, session: ReturnType<typeof memoryStorage>;
beforeEach(() => {
  local = memoryStorage();
  session = memoryStorage();
  Object.defineProperty(window, "localStorage", { configurable: true, value: local });
  Object.defineProperty(window, "sessionStorage", { configurable: true, value: session });
});

describe("persistedState stores", () => {
  it("keeps a value for the tab (sessionStorage) unless asked otherwise", () => {
    writePersisted("k", "v");
    expect(session.setItem).toHaveBeenCalledWith("k", '"v"');
    expect(local.setItem).not.toHaveBeenCalled();
    expect(readPersisted("k", "fallback", isString)).toBe("v");
  });

  it("keeps a value across restarts with store: local", () => {
    writePersisted("k", "v", "local");
    expect(local.setItem).toHaveBeenCalledWith("k", '"v"');
    expect(session.setItem).not.toHaveBeenCalled();
    expect(readPersisted("k", "fallback", isString, "local")).toBe("v");
    expect(readPersisted("k", "fallback", isString)).toBe("fallback"); // the other store does not have it
  });

  it("falls back for a value of the wrong shape, an unreadable one and a store that throws", () => {
    local.setItem("k", "5");
    expect(readPersisted("k", "fallback", isString, "local")).toBe("fallback");
    local.setItem("k", "{broken");
    expect(readPersisted("k", "fallback", isString, "local")).toBe("fallback");
    local.getItem.mockImplementation(() => { throw new Error("blocked"); });
    expect(readPersisted("k", "fallback", isString, "local")).toBe("fallback");
    local.setItem.mockImplementation(() => { throw new Error("quota"); });
    expect(() => writePersisted("k", "v", "local")).not.toThrow();
  });
});
