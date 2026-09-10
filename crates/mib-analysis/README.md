# Analysis helper supervisor

This crate owns private-pipe transport and the OS child for Toolkit page work.
The native `BackendFacade` operation ledger remains authoritative. Callers pass
its operation ID and dataset generation, handle the returned outcome, and check
generation again at publication. There is no second operation ledger here.

It is not yet wired to Tauri. The missing native/cxx adapter must register the
operation, bridge its cancellation flag to `cancel`, finish it exactly once and
classify interrupted/unknown operations at restart. UI commands must not accept
runtime paths, manifest digests, operation IDs or arbitrary helper methods.

## Verified bundle

`VerifiedBundle::load(root, expected_sha256, allow_development)` verifies the
manifest against a digest supplied by the trusted application build, and checks
the complete regular-file inventory and each SHA-256/size. The installer must
protect the installed directory against modification after verification; this
does not solve a concurrently mutable installation or replace OS code signing.
No symlinks, path traversal, unlisted import files, or system-Python fallback are
accepted. Verification is blocking local-file work, performed before supervision.

`manifest.json` schema 1 contains `distribution` (`development` or `production`),
`toolkit_version` (`0.1.0`), relative `interpreter`, `helper`, `toolkit_wheel`, and
`files`, a map of relative paths to `{sha256, size}`. The manifest excludes itself
from that map. Every runtime/library/import file in the bundle must be included.
The manifest is capped at 1 MiB; inventory at 20,000 entries, 32 directory levels,
and 4 GiB declared file bytes. The three required paths must be inventoried.

Production callers must pass `allow_development=false`. Non-Linux production is
currently rejected because Windows Job Object ownership at process creation is
not implemented. No production manifest/digest or release qualification is
asserted by this change. The test launchers use external Python solely to keep
fault-injection fixtures small; they are not redistributable runtime bundles.

## Lifecycle and budgets

`Supervisor::run` admits one native operation; a concurrent call returns `Busy`
without queuing. Each operation starts a fresh child, checks its bundle/wheel
identity handshake, runs one bounded calculation, closes stdin, and requires
clean EOF plus successful exit before returning its result. Python starts with
`-I -B`, a cleared environment (Windows retains `SystemRoot`), and the bundle
working directory. stderr is discarded, so it cannot block or grow retained logs;
bounded diagnostic retention is a remaining integration task.

Requests/replies are capped at 1 MiB; calculations at 4,096 values/pairs, 256
histogram bins or a 128×128 KDE grid with bandwidth 0.5–8. Caller deadlines are
positive and at most 60 seconds. A generation advance or matching operation
cancellation interrupts the child independently of pipe IO. Mismatched replies,
crashes, extra output, deadlines and cancellation cannot return successful data.
The cleanup wait is capped at two seconds; failed cleanup returns `CleanupUnknown`.

Dropping the request future requests process termination and lets Tokio reap it;
because the caller did not await cleanup, that supervisor becomes unavailable
(`CleanupUnknown`) for further launches. Other unconfirmed cleanup has the same
effect. Awaiting explicit `cancel`/generation invalidation is the normal path.
The application must resolve unknown state before constructing a replacement.

On Linux, a `PR_SET_PDEATHSIG` hook kills the helper on parent death; a post-hook
parent PID check closes the pre-registration race. Forced-parent-exit tests use
pidfd readiness to confirm termination. There are no descendant processes in the
supported helper protocol. This is process ownership, not a sandbox for arbitrary
untrusted Python code.

## Verification

Normal CI requires only Rust and an explicitly selected Python 3 interpreter:

```bash
MIB_ANALYSIS_TEST_PYTHON=/absolute/path/to/python3 \
  cargo test --locked --manifest-path crates/mib-analysis/Cargo.toml --tests
```

The installed Toolkit smoke test is explicitly ignored in public CI, which has
no private Toolkit checkout credential. To run it as well, select an interpreter
with the pinned Toolkit wheel installed and add `-- --include-ignored`. The
separate `desktop/analysis/test_helper.py` harness checks full Toolkit parity.

`.github/workflows/analysis-helper-ci.yml` runs public fault-injection tests in
normal and ThreadSanitizer lanes. The TSan command is:

```bash
RUSTFLAGS='-Zsanitizer=thread' MIB_ANALYSIS_TEST_PYTHON=/absolute/path/to/python3 \
  cargo +nightly-2026-09-10 test --locked --manifest-path crates/mib-analysis/Cargo.toml \
  -Zbuild-std --target x86_64-unknown-linux-gnu --tests
```

Install that toolchain with `rust-src`. Tests use a 60-second process watchdog,
bounded child waits, real OS processes, cancellation/generation stress, malformed
replies, crash-after-reply, stderr flood, digest/inventory rejection, dropped
futures and abrupt parent exit. No native camera/backend build is required.
