# Getting data off the board from the browser: a read-only file route (#651 G3)

Status: active

## Goal

Exports, experiment files and recordings stay on the board (RAM root today, SSD later); the browser has no way to
fetch them. Add a read-only listing and download route to `yofo-studio-server`, inside the same loopback/tunnel trust
as `/ws`, and use it for experiments, exports and (when it exists) the #649 buffer save. It also gives the
remote "path on the instrument" prompts a file browser (#651 G8).

## Design

**Routes** (`crates/mib-bridge-server/src/lib.rs`, beside `/ws`, `/auth`, `/healthz`; the same `authorized()` check, so a
token is required exactly when `/ws` requires one):

| Route | Result |
|---|---|
| `GET /files?path=<dir>` | JSON `{path, parent, writable, free_bytes, entries:[{name, kind: "file"\|"dir", size, mtime_ns}]}`, sorted dirs first then name; `path` omitted = the first root |
| `GET /files/download?path=<file>` | the file as `Content-Disposition: attachment`, with `Content-Length`, `Last-Modified` and **Range** support (resume over the tunnel), via `tower_http::services::ServeFile` after validation |

No upload, delete, rename or write of any kind: the route is read-only by construction (only `GET`, no body).

**Roots.** An allow-list: the data dir (`--data-dir`, default `/var/lib/yofo-studio`) plus any `--export-root DIR`
(repeatable; later the SSD mount). A request path is resolved to an absolute path, canonicalised (`std::fs::canonicalize`)
and must start with a canonical root: `..`, absolute paths outside the roots, and **symlinks that leave a root** are
refused (404, not 403, so existence outside the roots is not revealed). Names starting with `.` are not listed and not
served (the writable probes `.mib_hdf5_probe_*`, partial files). Only regular files and directories are returned
(no devices, sockets, FIFOs).

**Browser-origin rules.** The listing and download are plain `GET`s, so a page on another origin could trigger them
(the response is unreadable without CORS, which the server does not send, but a download is still a side effect and
the instrument is often reached without a token). Reject a request whose `Sec-Fetch-Site` is present and is not
`same-origin` or `none` (a typed URL or a link click from the Studio page); no CORS headers are added. This is the same
trust level as `/ws`; it is not a substitute for the token on a non-loopback listener.

**Load.** At most 2 concurrent downloads (the board is a 2-core A9 with a RAM root and the 5 kHz run on it); a third gets
`429`. Downloads read with the standard async file path; the server does not buffer a file in memory.

**UI.**
- A "Files" view (a panel on the Experiment tab for now): breadcrumb, entries with size and date, a Download link per
  file (a plain `<a href download>` to `/files/download?...`, so the browser's own download manager and resume apply),
  free space of the folder, and the "Recording to RAM" warning from `fetch_instrument_status.storage` when the folder is
  on the RAM root.
- The remote save prompt (`transport/dialogs.ts`) offers the same listing to pick a directory/name instead of
  typing a path (G8 left a text prompt).
- After an experiment finishes, the run outcome shows a Download link for its file; Export and (later) the #649 buffer
  save use the same link.

**Not in scope.** Writing, deleting or moving files; a general file manager; making the RAM root persistent (G2).

## Test plan

Rust tests in `crates/mib-bridge-server/tests/` (the existing `ws.rs` harness, a temporary data dir):
listing and sort order; download with `Content-Length` and a `Range` request; `..` traversal, an absolute path outside the
roots and a symlink out of a root are all 404; dot-files are neither listed nor served; the token rule matches `/ws`
(401 without it when a token is set); a cross-site `Sec-Fetch-Site` is refused; the third concurrent download is 429; a
POST/PUT is 405. UI: a vitest for the breadcrumb/entry model and the download href. Board check in a slot: list the data
dir, download a finished experiment over the tunnel and open it in YOFO Review on the PC.

## Open points

1. Is `Sec-Fetch-Site` enough, or should downloads additionally need an anti-CSRF token from `/auth`? (The route is read-only.)
2. Root layout once the SSD exists (#651 G2): one `--export-root` per mount, or the data dir moves.
3. Should `GET /files` also report whether a file is still being written (an open experiment file), so the UI does not offer a
   half-finished download? (Today: the experiment file is only complete after Stop.)

## Progress

- [x] design (this note)
- [ ] server route + tests
- [ ] UI files view and the post-run Download link
- [ ] board check
