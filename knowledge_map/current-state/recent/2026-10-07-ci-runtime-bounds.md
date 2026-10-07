## 2026-10-07 — Bound CI runtime and preserve diagnostic coverage

Linux sanitizer, backend, and nightly soak workflows now have explicit job and
step budgets, GNU timeout escalation (`TERM`, then `KILL` after 30 seconds),
always-run bounded logs, and sccache diagnostics. The workflows use the
official `mozilla-actions/sccache-action@v0.0.11` with sccache `v0.16.0`; the
existing CMake launcher auto-detection remains authoritative. Soak repeats are
validated before setup (1–100, default 40), and stale runs on the same ref are
cancelled. The Python wheel matrix keeps all existing builds and tests while
giving the QEMU ARM64 entry a 120-minute cap versus 20 minutes for x86_64.

The selected limits are documented in [[build-and-run/Build]] and
[the testing strategy](../../../docs/architecture/testing-strategy.md).
Evidence includes [backend run 37583515274](https://github.com/gavinlouuu-kpt/mib-studio-qt/actions/runs/37583515274),
[sanitizer run 37582104010](https://github.com/gavinlouuu-kpt/mib-studio-qt/actions/runs/37582104010),
[sanitizer run 37562107549](https://github.com/gavinlouuu-kpt/mib-studio-qt/actions/runs/37562107549),
[soak run 37456065522](https://github.com/gavinlouuu-kpt/mib-studio-qt/actions/runs/37456065522),
and [ARM64 wheel run 37568147020](https://github.com/gavinlouuu-kpt/mib-studio-qt/actions/runs/37568147020).
These runs show queue time,
retry/cancellation churn, and QEMU cost, but do not prove a deadlock or a
complete billing total. Hosted cache hit behavior remains to be verified by a
fresh run.
