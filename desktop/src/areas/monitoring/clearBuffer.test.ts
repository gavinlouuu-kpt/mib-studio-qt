import { describe, expect, it, vi } from "vitest";
import { CLEAR_BUFFER_QUESTION, clearMonitoringBuffer } from "./clearBuffer";

const setup = (over: Partial<Parameters<typeof clearMonitoringBuffer>[0]> = {}) => {
  const d = {
    ask: vi.fn(async () => true), clear: vi.fn(async () => ({ ok: true, message: "" })), onCleared: vi.fn(), append: vi.fn(), ...over,
  };
  return d;
};

describe("Clear Buffer (MON-029)", () => {
  it("asks Qt's question first and clears nothing on No", async () => {
    const d = setup({ ask: vi.fn(async () => false) });
    expect(await clearMonitoringBuffer(d)).toBe("declined");
    expect(d.ask).toHaveBeenCalledWith(CLEAR_BUFFER_QUESTION, { title: "Clear Buffer", kind: "warning" });
    expect(d.clear).not.toHaveBeenCalled();
    expect(d.onCleared).not.toHaveBeenCalled();
    expect(CLEAR_BUFFER_QUESTION).toBe("Are you sure you want to clear the monitoring buffer? This will remove all accumulated frames.");
  });
  it("clears on Yes, then tells the listeners (KDE state and the selection of later PRs) and logs", async () => {
    const d = setup();
    expect(await clearMonitoringBuffer(d)).toBe("cleared");
    expect(d.clear).toHaveBeenCalledTimes(1);
    expect(d.onCleared).toHaveBeenCalledTimes(1);
    expect(d.append).toHaveBeenCalledWith("monitoring buffers cleared");
  });
  it("keeps the listeners quiet when the backend refuses or fails", async () => {
    const refused = setup({ clear: vi.fn(async () => ({ ok: false, message: "busy" })) });
    expect(await clearMonitoringBuffer(refused)).toBe("failed");
    expect(refused.append).toHaveBeenCalledWith("clear failed: busy");
    expect(refused.onCleared).not.toHaveBeenCalled();
    const thrown = setup({ clear: vi.fn(async () => { throw new Error("gone"); }) });
    expect(await clearMonitoringBuffer(thrown)).toBe("failed");
    expect(thrown.onCleared).not.toHaveBeenCalled();
  });
  it("treats a question that cannot be shown as No", async () => {
    const d = setup({ ask: vi.fn(async () => { throw new Error("no window"); }) });
    expect(await clearMonitoringBuffer(d)).toBe("declined");
    expect(d.clear).not.toHaveBeenCalled();
    expect(d.append).toHaveBeenCalledWith(expect.stringContaining("nothing was cleared"));
  });
});
