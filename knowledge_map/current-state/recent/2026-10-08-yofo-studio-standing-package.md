## 2026-10-08 — The standing YOFO Studio package for the PZ7035

`scripts/yofo/package_studio.sh` stages the stripped armv7 server, `dist.tar`, a systemd unit
(`deploy/yofo-studio/yofo-studio.service`: loopback `127.0.0.1:8427`, `--no-token`, Aravis/GenTL,
`ExecStartPre` waits for the PL via `pl-ready.sh`, Restart=on-failure), `install.sh`, `MD5SUMS` and
`BUILD_INFO` on the HDD; the board owner installs it after each Linux boot, replacing the bench
viewer as the board's standing state. Checked: the UI connects without a prompt against a
`--no-token` loopback server (Chromium). See [[build-and-run/Build]], [[architecture/Desktop-Shell]].
