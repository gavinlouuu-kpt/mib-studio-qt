# Agent B frame-transaction hardening

Inventory, candidate SHAs, ownership requests and acceptance status live in
`docs/exec-plans/active/2026-09-07-agent-b-react-tauri.md`.

The first deterministic regression in `desktop/src/frameTransaction.test.ts`
executes the production TypeScript bridge client against a command-boundary
fake matching the native split cache. Before the fix, interleaving an indexed
pull replaces live pixels: [22,22,22,22] instead of [11,11,11,11]. This is client
transport evidence, not a native Tauri end-to-end pass.

Related: [[../architecture/Rust-Bridge]], [[../architecture/Desktop-Shell]].

Implemented: atomic response, generated packet bounds, exact frame identities,
no Rust image cache, stale reply guards and bounded live/Review ownership.
99 frontend tests and production build pass; Rust tests/native E2E/Qt comparison
are unexecuted. The 10,000-pull and 100-view-cycle tests exercise transport
ownership, not hardware throughput. Complete contract and resource limitations:
`docs/architecture/frame-packet-v1.md`.
