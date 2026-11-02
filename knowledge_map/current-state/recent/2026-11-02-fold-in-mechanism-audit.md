## 2026-11-02 — Fold-in audit: mechanism first, GenApi-driven eGrabber (#806, #807, #808)

Audited every open fold-in branch. Most I-track PRs are UI only. The F track has a
real native evaluator but caps sources at 10,000 events / 64 MiB. E's typed feature
control (#812) has no caller, and nothing enumerates the GenApi node map. I (#815)
and F (#810) carry diverging copies of the analysis code, and both claim bridge ABI 47.

The plan sets one definition of done: a ledger row is accepted only with a native
authority, a bridge contract, production wiring, a real-backend test and a stated
evidence class. The eGrabber objective is now explicit: the camera is driven
through its GenApi node map at runtime, not a hand-kept feature list.

See `docs/exec-plans/active/2026-11-02-fold-in-mechanism-realignment.md`.
