import { useCallback, useEffect, useState } from "react";
import { breadcrumbs, childPath, downloadUrl, fetchListing, formatSize, type FileListing } from "../filesView";
import { serverOrigin, tokenFromLocation } from "../transport/auth";

// The instrument's data folder, read-only (#651 G3): browse, and download through the browser's own
// download manager (a plain link). A file an experiment is still writing is listed but not offered.
export function FilesPanel({ ramWarning }: { ramWarning?: string }) {
  const [path, setPath] = useState<string | undefined>(undefined);
  const [listing, setListing] = useState<FileListing | null>(null);
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  const origin = serverOrigin();
  const token = tokenFromLocation();

  const load = useCallback(async (target: string | undefined) => {
    setBusy(true);
    try {
      const next = await fetchListing(origin, target, token);
      setListing(next);
      setPath(next.path);
      setError("");
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    } finally {
      setBusy(false);
    }
  }, [origin, token]);

  useEffect(() => { void load(undefined); }, [load]);

  const crumbs = listing ? breadcrumbs(listing.root, listing.path) : [];
  return (
    <section className="files-panel" aria-label="Instrument files">
      <div className="toolbar">
        <span className="crumbs">
          {crumbs.map((c, i) => (
            <span key={c.path}>
              {i > 0 && " / "}
              <button className="btn small link" onClick={() => void load(c.path)} disabled={busy || c.path === listing?.path}>{c.label}</button>
            </span>
          ))}
        </span>
        <button className="btn small" onClick={() => void load(path)} disabled={busy}>Refresh</button>
      </div>
      {ramWarning && <p className="pending-note" role="status">{ramWarning}</p>}
      {error && <p role="alert">{error}</p>}
      {listing && (
        <table className="metrics-table">
          <thead><tr><th>Name</th><th>Size</th><th>Modified</th><th /></tr></thead>
          <tbody>
            {listing.parent !== null && (
              <tr><td colSpan={4}><button className="btn small link" onClick={() => void load(listing.parent ?? undefined)}>..</button></td></tr>
            )}
            {listing.entries.length === 0 && <tr><td colSpan={4} className="dim">This folder is empty.</td></tr>}
            {listing.entries.map((e) => {
              const full = childPath(listing.path, e.name);
              return (
                <tr key={e.name}>
                  <td>
                    {e.kind === "dir"
                      ? <button className="btn small link" onClick={() => void load(full)}>{e.name}/</button>
                      : e.name}
                  </td>
                  <td>{e.kind === "file" ? formatSize(e.size) : ""}</td>
                  <td>{e.mtime_ns ? new Date(e.mtime_ns / 1e6).toLocaleString() : ""}</td>
                  <td>
                    {e.kind === "file" && (e.in_progress
                      ? <span className="dim" title="An experiment is still writing this file">being written</span>
                      : <a href={downloadUrl(origin, full, token)} download={e.name}>Download</a>)}
                  </td>
                </tr>
              );
            })}
          </tbody>
        </table>
      )}
    </section>
  );
}
