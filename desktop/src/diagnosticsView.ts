// The Diagnostics view model (#651 G12): GET /diagnostics of yofo-studio-server, read-only.

export interface Diagnostics {
  server: { version: string; boot_id: string; uptime_s: number };
  bundle: string | null;
  data_dir: string;
  log: { path: string; size: number; tail: string[]; tail_truncated: boolean } | null;
  key_lines: string[];
}

export type LogLevel = "all" | "warning" | "error";

export const diagnosticsUrl = (origin: string, lines: number, token: string): string =>
  `${origin}/diagnostics?lines=${lines}${token ? `&token=${encodeURIComponent(token)}` : ""}`;

/** spdlog lines look like `[2026-10-09 10:01:00.123] [app] [warning] [File.cpp:12] text`. */
export function lineLevel(line: string): "error" | "warning" | "info" {
  const m = /\]\s*\[(trace|debug|info|warning|error|critical)\]/.exec(line);
  if (!m) return "info";
  if (m[1] === "error" || m[1] === "critical") return "error";
  return m[1] === "warning" ? "warning" : "info";
}

export function filterLines(lines: string[], level: LogLevel): string[] {
  if (level === "all") return lines;
  return lines.filter((l) => {
    const v = lineLevel(l);
    return level === "warning" ? v === "warning" || v === "error" : v === "error";
  });
}

export function formatUptime(seconds: number): string {
  if (!Number.isFinite(seconds) || seconds < 0) return "—";
  const d = Math.floor(seconds / 86400), h = Math.floor((seconds % 86400) / 3600), m = Math.floor((seconds % 3600) / 60);
  return d ? `${d} d ${h} h` : h ? `${h} h ${m} min` : m ? `${m} min` : `${Math.floor(seconds)} s`;
}

/** The text the Copy button puts on the clipboard: attach it to an issue as it is. */
export function diagnosticsText(d: Diagnostics, instrumentLines: string[], level: LogLevel): string {
  const parts = [
    d.bundle ?? "(no bundle line: not installed from a bundle)",
    `server ${d.server.version}, up ${formatUptime(d.server.uptime_s)}, data dir ${d.data_dir}`,
    ...instrumentLines,
    "",
    "-- start-up and mode lines --",
    ...d.key_lines,
    "",
    d.log ? `-- ${d.log.path} (${filterLines(d.log.tail, level).length} lines, ${level}) --` : "-- no log file --",
    ...(d.log ? filterLines(d.log.tail, level) : []),
  ];
  return parts.join("\n");
}
