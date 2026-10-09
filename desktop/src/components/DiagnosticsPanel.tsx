import { useCallback, useEffect, useState } from "react";
import { diagnosticsText, diagnosticsUrl, filterLines, formatUptime, type Diagnostics, type LogLevel } from "../diagnosticsView";
import { downloadUrl } from "../filesView";
import { serverOrigin, tokenFromLocation } from "../transport/auth";

// Board diagnostics without a shell (#651 G12): versions, the installed bundle, the PL identity from the
// instrument status, the start-up lines and the log tail; Copy for an issue, Download for the whole log.
export function DiagnosticsPanel({ instrumentLines, volatileNotice }: { instrumentLines: string[]; volatileNotice?: string | null }) {
  const [data, setData] = useState<Diagnostics | null>(null);
  const [error, setError] = useState("");
  const [level, setLevel] = useState<LogLevel>("all");
  const [copied, setCopied] = useState(false);
  const origin = serverOrigin();
  const token = tokenFromLocation();

  const load = useCallback(async () => {
    try {
      const response = await fetch(diagnosticsUrl(origin, 200, token), { cache: "no-store" });
      if (!response.ok) throw new Error(response.status === 401 ? "The instrument wants its access token." : `The instrument answered ${response.status}.`);
      setData((await response.json()) as Diagnostics);
      setError("");
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    }
  }, [origin, token]);

  useEffect(() => { void load(); }, [load]);

  const copy = async () => {
    if (!data) return;
    try {
      await navigator.clipboard.writeText(diagnosticsText(data, instrumentLines, level));
      setCopied(true);
      window.setTimeout(() => setCopied(false), 2000);
    } catch {
      setError("Copy is not available in this browser; select the text instead.");
    }
  };

  const shown = data?.log ? filterLines(data.log.tail, level) : [];
  return (
    <section className="diagnostics-panel" aria-label="Diagnostics">
      <div className="toolbar">
        <button className="btn small" onClick={() => void load()}>Refresh</button>
        <button className="btn small" onClick={() => void copy()} disabled={!data}>{copied ? "Copied" : "Copy"}</button>
        {data?.log && <a href={downloadUrl(origin, data.log.path, token)} download="app.log">Download log</a>}
        <label>Level{" "}
          <select value={level} onChange={(e) => setLevel(e.target.value as LogLevel)}>
            <option value="all">all</option><option value="warning">warnings and errors</option><option value="error">errors</option>
          </select>
        </label>
      </div>
      {volatileNotice && <p className="pending-note" role="alert" data-testid="volatile-data">{volatileNotice}</p>}
      {error && <p role="alert">{error}</p>}
      {data && (
        <>
          <p className="mono">{data.bundle ?? "No bundle line: not installed from a bundle."}</p>
          <p className="mono">server {data.server.version} · up {formatUptime(data.server.uptime_s)} · data dir {data.data_dir}</p>
          {instrumentLines.map((l) => <p className="mono" key={l}>{l}</p>)}
          <h5>Start-up and mode lines</h5>
          <pre className="script-editor" style={{ maxHeight: 160, overflow: "auto" }}>{data.key_lines.join("\n") || "none yet"}</pre>
          <h5>{data.log ? `Log tail (${shown.length} of ${data.log.tail.length} lines, ${level})` : "No log file yet"}</h5>
          {data.log && <pre className="script-editor" style={{ maxHeight: 320, overflow: "auto" }}>{shown.join("\n")}</pre>}
        </>
      )}
    </section>
  );
}
