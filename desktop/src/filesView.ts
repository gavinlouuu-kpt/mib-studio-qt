// The instrument's files, as the browser sees them (#651 G3): the read-only GET /files and
// GET /files/download of yofo-studio-server, one root (the data dir). Pure helpers + one fetch.

export interface FileEntry {
  name: string;
  kind: "file" | "dir";
  size: number;
  mtime_ns: number;
  /** An experiment is still writing it: listed, not downloadable. */
  in_progress: boolean;
}

export interface FileListing {
  root: string;
  path: string;
  parent: string | null;
  entries: FileEntry[];
}

const query = (path: string | undefined, token: string): string => {
  const parts: string[] = [];
  if (path) parts.push(`path=${encodeURIComponent(path)}`);
  if (token) parts.push(`token=${encodeURIComponent(token)}`);
  return parts.length ? `?${parts.join("&")}` : "";
};

export const listUrl = (origin: string, path: string | undefined, token: string): string =>
  `${origin}/files${query(path, token)}`;

/** A plain link target: the browser's own download manager (and resume) does the rest. */
export const downloadUrl = (origin: string, path: string, token: string): string =>
  `${origin}/files/download${query(path, token)}`;

/** `dir` joined with an entry name, as the server resolves it (absolute paths are fine). */
export const childPath = (dir: string, name: string): string => `${dir.replace(/\/+$/, "")}/${name}`;

export function formatSize(bytes: number): string {
  if (!Number.isFinite(bytes) || bytes < 0) return "—";
  if (bytes < 1024) return `${bytes} B`;
  const units = ["KiB", "MiB", "GiB", "TiB"];
  let value = bytes / 1024;
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit += 1;
  }
  return `${value >= 100 ? value.toFixed(0) : value.toFixed(1)} ${units[unit]}`;
}

/** Crumbs from the root to `path`: [{label, path}], the root first. */
export function breadcrumbs(root: string, path: string): { label: string; path: string }[] {
  const crumbs = [{ label: "data", path: root }];
  if (!path.startsWith(root) || path === root) return crumbs;
  let current = root;
  for (const part of path.slice(root.length).split("/").filter(Boolean)) {
    current = childPath(current, part);
    crumbs.push({ label: part, path: current });
  }
  return crumbs;
}

export async function fetchListing(
  origin: string,
  path: string | undefined,
  token: string,
  fetchImpl: typeof fetch = fetch,
): Promise<FileListing> {
  const response = await fetchImpl(listUrl(origin, path, token), { cache: "no-store" });
  if (response.status === 401) throw new Error("The instrument wants its access token.");
  if (response.status === 404) throw new Error("That folder is not available.");
  if (!response.ok) throw new Error(`The instrument answered ${response.status}.`);
  return (await response.json()) as FileListing;
}
