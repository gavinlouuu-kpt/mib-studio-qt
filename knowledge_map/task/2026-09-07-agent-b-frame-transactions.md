# Agent B frame-transaction hardening

Inventory, candidate SHAs, ownership requests and acceptance status live in
`docs/exec-plans/active/2026-09-07-agent-b-react-tauri.md`.

The first deterministic regression in `desktop/src/frameTransaction.test.ts`
executes the production TypeScript bridge client against a command-boundary
fake matching the native split cache. Before the fix, interleaving an indexed
pull replaces live pixels: [22,22,22,22] instead of [11,11,11,11]. This is client
transport evidence, not a native Tauri end-to-end pass.

Related: [[../architecture/Rust-Bridge]], [[../architecture/Desktop-Shell]].
