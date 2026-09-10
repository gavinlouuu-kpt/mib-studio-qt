# Analysis helper process ownership

**Source:** `crates/mib-analysis/`, `desktop/analysis/`.
**Status:** independently tested; not wired into [[Desktop-Shell]].
**Native operation owner:** [[AppBackend]] / BackendFacade via [[Rust-Bridge]].

`VerifiedBundle::load` checks a caller-supplied trusted manifest digest and every
runtime file hash/size, rejecting traversal, symlinks and extra imports. Installer
protection is required after verification; hashing does not replace code signing
or protection against concurrent installation edits. Production bundles are
currently accepted only on Linux; other platforms require qualified parent
process ownership first.

`Supervisor::run` takes the native operation ID and dataset generation, admits
one operation without queuing, and starts a fresh child for a handshake and one
Toolkit histogram/KDE page. The child uses private pipes, `-I -B`, a cleared
environment and bundle-relative paths. JSON is capped at 1 MiB; values/pairs at
4,096. stderr is discarded rather than retained unboundedly.

Cancellation and generation replacement interrupt the child independently of
its computation pipe. Deadlines cover IO, computation and normal exit; cleanup
has a separate two-second limit. Replies require exact identity and a successful
process exit. Unknown cleanup or dropping the request future makes the supervisor
unavailable until the application resolves its unknown state. Linux parent-death
handling uses `PR_SET_PDEATHSIG` and a race check.

The crate does not invent operation IDs or durable state. The native/cxx adapter
must register jobs, forward cancellation and finish the native ledger exactly
once before the webview gains analysis commands. Windows Job Objects, signed
runtime packaging, bounded diagnostic logs, restart classification and production
qualification remain open. See [[../task/2026-09-10-local-analysis]] and
`crates/mib-analysis/README.md` for executed gates and test commands.
