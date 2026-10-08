## 2026-10-09 — Diagnostics view without a shell (#651 G12)

`GET /diagnostics?lines=` on yofo-studio-server (read-only; the token and cross-site rules of `/files`) returns the
server version and uptime, the installed bundle line (`install.sh` leaves `BUILD_INFO` next to the UI), the data dir, the
tail of `<data>/logs/app.log` (size-capped: at most 500 lines from the last 256 KiB; invalid UTF-8 replaced) and the
start-up key lines (E-modulus LUT, PL results/execution provider, RXH1, mode switches, Align preview), searched in the last
2 MiB. The browser's Settings menu has **Diagnostics…**: the same fields plus the PL identity, sensor and storage from the
instrument status, a level filter, **Copy** (a text to paste into an issue) and **Download log** (the G3 route). No shell
needed; nothing is written. See [[Rust-Bridge]] and [[Desktop-Shell]].
