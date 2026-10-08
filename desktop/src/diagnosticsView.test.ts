import { describe, expect, it } from "vitest";
import { diagnosticsText, diagnosticsUrl, filterLines, formatUptime, lineLevel, type Diagnostics } from "./diagnosticsView";

const lines = [
  "[2026-10-09 10:00:00.001] [app] [info] [A.cpp:1] fine",
  "[2026-10-09 10:00:01.002] [app] [warning] [B.cpp:2] careful",
  "[2026-10-09 10:00:02.003] [app] [error] [C.cpp:3] broken",
  "a continuation line without a level",
];

describe("diagnostics view model (#651 G12)", () => {
  it("reads spdlog levels and filters", () => {
    expect(lines.map(lineLevel)).toEqual(["info", "warning", "error", "info"]);
    expect(filterLines(lines, "all")).toHaveLength(4);
    expect(filterLines(lines, "warning")).toEqual([lines[1], lines[2]]);
    expect(filterLines(lines, "error")).toEqual([lines[2]]);
  });
  it("builds the URL and formats uptime", () => {
    expect(diagnosticsUrl("http://b:8427", 200, "")).toBe("http://b:8427/diagnostics?lines=200");
    expect(diagnosticsUrl("http://b:8427", 50, "a b")).toBe("http://b:8427/diagnostics?lines=50&token=a%20b");
    expect(formatUptime(45)).toBe("45 s");
    expect(formatUptime(3600 * 5 + 120)).toBe("5 h 2 min");
    expect(formatUptime(86400 * 2 + 3600)).toBe("2 d 1 h");
    expect(formatUptime(-1)).toBe("—");
  });
  it("makes the copy text: bundle line, server, instrument lines, key lines and the filtered tail", () => {
    const d: Diagnostics = {
      server: { version: "0.1.0", boot_id: "b", uptime_s: 90 },
      bundle: "YOFO Studio bundle: mib-studio-qt abc + pz7035-imx426 def",
      data_dir: "/var/lib/yofo-studio",
      log: { path: "/var/lib/yofo-studio/logs/app.log", size: 10, tail: lines, tail_truncated: false },
      key_lines: ["AppBackend: RXH1 v2: CTRL left at 0x64140801"],
    };
    const text = diagnosticsText(d, ["PL build 00043db7"], "warning");
    expect(text.split("\n")[0]).toContain("YOFO Studio bundle");
    expect(text).toContain("up 1 min");
    expect(text).toContain("PL build 00043db7");
    expect(text).toContain("RXH1 v2");
    expect(text).toContain("careful");
    expect(text).not.toContain("fine");
    expect(diagnosticsText({ ...d, bundle: null, log: null }, [], "all")).toContain("no log file");
  });
});
