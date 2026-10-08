## 2026-10-09 — Read-only file listing and download in yofo-studio-server (#651 G3)

`GET /files?path=` lists a folder under the data dir (name, kind, size, mtime, `in_progress`) and
`GET /files/download?path=` serves a file with `Content-Length`, `Last-Modified`, `Content-Disposition: attachment`
and Range (resume over the tunnel). One root, the data dir: every request path is canonicalised and must stay under it,
so `..`, absolute paths elsewhere and symlinks out of the root are 404; dot-names are neither listed nor served; the
output file of a run that is still writing is listed `in_progress` and refused (409); at most 2 downloads at once
(429); the token rule of `/ws`; a request with `Sec-Fetch-Site` other than same-origin/none is refused (403); no write
verbs (405). Integration tests in `crates/mib-bridge-server/tests/files.rs`. The Files view in the UI follows. See
[[Rust-Bridge]].
