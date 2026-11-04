// SerialReads (#736 review): one read at a time, a stuck read stops holding the poll after
// staleMs, and an older reply never overwrites a newer one.
import { describe, expect, it } from "vitest";
import { SerialReads } from "./serialReads";

describe("SerialReads", () => {
  it("skips a tick while a read is in flight, and starts the next once it ended", () => {
    let t = 0;
    const r = new SerialReads(10_000, () => t);
    const a = r.begin();
    expect(a).toBe(1);
    for (t = 1500; t < 9000; t += 1500) expect(r.begin()).toBeNull();
    expect(r.busy).toBe(true);
    r.end(a!);
    expect(r.busy).toBe(false);
    expect(r.begin()).toBe(2);
  });

  it("a read stuck for staleMs no longer holds the poll; its late end frees nothing", () => {
    let t = 0;
    const r = new SerialReads(10_000, () => t);
    const a = r.begin()!;
    t = 9_999;
    expect(r.begin()).toBeNull();
    t = 10_000;
    const b = r.begin()!;
    expect(b).toBe(2);
    r.end(a); // the stale read ends late: the newer one still holds the poll
    expect(r.busy).toBe(true);
    expect(r.begin()).toBeNull();
    r.end(b);
    expect(r.begin()).toBe(3);
  });

  it("drops a reply older than one already applied; parts of one read are all accepted", () => {
    let t = 0;
    const r = new SerialReads(10_000, () => t);
    const a = r.begin()!;
    t = 10_000;
    const b = r.begin()!;
    expect(r.accept(b)).toBe(true);
    expect(r.accept(b)).toBe(true); // a second part of the same read
    expect(r.accept(a)).toBe(false); // the older reply arrives last: dropped
    r.end(b);
    r.end(a);
    const c = r.begin()!;
    expect(r.accept(c)).toBe(true);
  });

  it("an older reply that arrives first is applied, then replaced by the newer one", () => {
    let t = 0;
    const r = new SerialReads(1000, () => t);
    const a = r.begin()!;
    t = 1000;
    const b = r.begin()!;
    expect(r.accept(a)).toBe(true);
    expect(r.accept(b)).toBe(true);
  });
});
