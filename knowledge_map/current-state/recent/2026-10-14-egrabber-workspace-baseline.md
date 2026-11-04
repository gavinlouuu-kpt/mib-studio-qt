## 2026-10-14 — eGrabber-inspired workspace branch baseline

The `feat/egrabber-workspace` branch starts from develop commit
`2a4e0a4f38055df5464400048ab9948d949319e2`. Its initial commit adds the
reviewed v2 offline interaction reference, complete workspace design brief and
staged implementation plan. It does not replace or claim to have implemented
the live shell, hardware control, bridge DTOs or persistence behavior.

Start with `EGRABBER_WORKSPACE.md` at the repository root, then
`docs/design/egrabber-workspace/README.md` and
`docs/exec-plans/active/2026-10-14-egrabber-workspace.md`. Reuse
[[architecture/Desktop-Shell]], [[architecture/Rust-Bridge]],
[[architecture/AppBackend]] and [[architecture/ExperimentCoordinator]].

The full target remains Prepare / Align / Experiment / Review, including LED,
motor/piezo, storage, software readiness and linked scatter evidence. The seven
HTML views cover later engineering refinements only. Keep simulated readbacks,
E curves and state transitions outside production paths; unsupported backend
capabilities remain explicit. The public reference excludes private Penpot
endpoints and recovery operations. Native Penpot links remain unverified.
