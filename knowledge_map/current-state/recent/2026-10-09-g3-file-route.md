## 2026-10-09 — Read-only file listing and download in yofo-studio-server (#651 G3)

`GET /files?path=` lists a folder under the data dir (name, kind, size, mtime, `in_progress`) and
`GET /files/download?path=` serves a file with `Content-Length`, `Last-Modified`, `Content-Disposition: attachment`
and Range (resume over the tunnel). One root, the data dir: every request path is canonicalised and must stay under it,
so `..`, absolute paths elsewhere and symlinks out of the root are 404; dot-names are neither listed nor served; the
output file of a run that is still writing is listed `in_progress` and refused (409); at most 2 downloads at once
(429); the token rule of `/ws`; a request whose `Origin` is not the server's own (its `Host`) or an `--allow-origin` is refused (403), on `/ws` as well
(Fetch Metadata is not sent over plain HTTP to a Tailscale address, so `Origin` is the gate), and `Sec-Fetch-Site`, when
sent, must be same-origin or none; no write verbs (405). Hardening from the Codex review: files and directories are opened
beneath the root descriptor with `openat2` (`RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS`, `O_NONBLOCK`) and `fstat` must say
regular file (a FIFO is refused without blocking), the same descriptor is streamed with the module's own single-range
handling (a comma or a header over 64 bytes is 416), listings are paged (2000 entries, `offset`/`limit`, `truncated`) and
limited to 2 concurrent scans (429), and the active run's output path is resolved against the process working directory
like the writer does. Integration tests in `crates/mib-bridge-server/tests/files.rs`. The Files view in the UI follows. See
[[Rust-Bridge]].
