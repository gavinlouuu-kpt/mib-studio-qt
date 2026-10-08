import { describe, expect, it, vi } from "vitest";
import { breadcrumbs, childPath, downloadUrl, fetchListing, formatSize, listUrl } from "./filesView";

describe("files view model (#651 G3)", () => {
  it("builds list and download URLs with the path and token encoded", () => {
    expect(listUrl("http://b:8427", undefined, "")).toBe("http://b:8427/files");
    expect(listUrl("http://b:8427", "/var/lib/yofo-studio/runs", "t k")).toBe("http://b:8427/files?path=%2Fvar%2Flib%2Fyofo-studio%2Fruns&token=t%20k");
    expect(downloadUrl("http://b:8427", "run 1.h5", "")).toBe("http://b:8427/files/download?path=run%201.h5");
  });
  it("joins names and formats sizes", () => {
    expect(childPath("/data/", "a.h5")).toBe("/data/a.h5");
    expect(formatSize(10)).toBe("10 B");
    expect(formatSize(1536)).toBe("1.5 KiB");
    expect(formatSize(288 * 1024 * 1024)).toBe("288 MiB");
    expect(formatSize(-1)).toBe("—");
  });
  it("makes breadcrumbs from the root", () => {
    expect(breadcrumbs("/data", "/data")).toEqual([{ label: "data", path: "/data" }]);
    expect(breadcrumbs("/data", "/data/runs/2026")).toEqual([
      { label: "data", path: "/data" }, { label: "runs", path: "/data/runs" }, { label: "2026", path: "/data/runs/2026" },
    ]);
  });
  it("turns the server's refusals into plain messages", async () => {
    const reply = (status: number, body: unknown = {}) => vi.fn().mockResolvedValue({ status, ok: status < 300, json: async () => body });
    await expect(fetchListing("http://b", undefined, "", reply(401) as never)).rejects.toThrow("access token");
    await expect(fetchListing("http://b", "x", "", reply(404) as never)).rejects.toThrow("not available");
    await expect(fetchListing("http://b", "x", "", reply(500) as never)).rejects.toThrow("500");
    const listing = { root: "/data", path: "/data", parent: null, entries: [] };
    await expect(fetchListing("http://b", undefined, "", reply(200, listing) as never)).resolves.toEqual(listing);
  });
});
