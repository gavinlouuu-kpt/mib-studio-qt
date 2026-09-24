# Golden Principles — MIB Studio Qt

Opinionated, mechanical rules that keep this repo legible and consistent for
agent runs. When a principle is violated, fix it in the same PR or log it in
[`exec-plans/tech-debt-tracker.md`](exec-plans/tech-debt-tracker.md). When a
principle is repeatedly violated, promote it into a linter or CI check.

## 1. The repo is the system of record

Decisions, architecture knowledge, and operational context must live in
`knowledge_map/` or `docs/` — not in chat threads or heads. The vault note is
updated in the same commit as the code it describes (see
[`../knowledge_map/Vault-Maintenance.md`](../knowledge_map/Vault-Maintenance.md)).
Significant design choices get an ADR in [`decisions/`](decisions/README.md).

## 2. The backend/frontend boundary is a contract

`src/frontend/` depends on `src/backend/` through `AppBackend` /
`BackendFacade` only — never the reverse, and never frontend code reaching
into service internals. The backend stays Qt-Widgets-free so
`linux-backend-only` builds and tests keep passing. See
[`architecture/backend-boundaries.md`](architecture/backend-boundaries.md).

## 3. Services own a single concern

New behavior goes into the service that owns the concern, or a new service
wired through `AppBackend` — not into an existing service "because it was
nearby". Each service gets a vault note under `knowledge_map/services/`.

## 4. Threading rules are explicit

UI work on the main thread; capture, processing, autofocus, and trigger work
on their dedicated threads as documented in the vault threading-model note.
Cross-thread handoff happens via the established patterns (ring buffer,
callbacks, atomics) — do not invent a new one without an ADR.

## 5. Logging is spdlog, always

Never `std::cout`/`std::cerr`/`qDebug` in app code. Log lifecycle events at
info, recoverable anomalies at warn, with enough context to debug from the
log file alone — agents and humans both read logs as a feedback loop.

## 6. Prefer existing utilities and SDK patterns

Check `Tools` (`src/backend/app/Tools.cpp`) before writing helpers. Reference
`egrabber-sample-programs` before implementing camera functions. Replicating
an established pattern keeps invariants centralized; a parallel hand-rolled
one creates drift that future agents will copy.

## 7. Headers mirror sources

Every `src/<area>/Foo.cpp` has its header at `include/<area>/Foo.h`. New code
follows the layout; moved code moves both halves.

## 8. Verification is mechanical

```bash
python3 scripts/check_docs.py             # docs + vault wikilink integrity
ctest --preset linux-backend-only-test    # backend unit tests
```

Format new/edited C++ with `clang-format` (config at repo root). Performance
experiments log all metrics to MLflow (`mlflow.yofo.bio`), credentials via
env vars only.

## 9. Plans are first-class artifacts

Multi-PR or design-heavy work gets an execution plan in
[`exec-plans/active/`](exec-plans/active/) with a status and decision log.
Known debt goes to the tracker — never into silent TODOs.

## 10. Runtime data stays out of git

Logs, sqlite, HDF5 output, and mock frames live under `data/` (gitignored).
Never commit experiment data, credentials, or machine-local paths.

## 11. Credentials never enter the tree

Secrets come from environment variables or CI secrets at the moment they are
used — never from a tracked file, and never as a literal inside a command
that gets recorded (agent permission allowlists such as
`.claude/settings.local.json` are per-machine and gitignored for this
reason). If a credential lands in git, rotate it first; removing the file
or rewriting history does not un-publish it. Before committing, a quick
`git grep -nE '(PASSWORD|TOKEN|SECRET|API_KEY)=' -- ':!*.md'` should return
only `$VAR`, `os.environ`, or `secrets.` references.

## 12. Every setup list has exactly one home

Package names, tool minimums, Python requirements, Conan profiles and
external assets each live in one machine-readable file under `env/` or
`conan/profiles/` (`knowledge_map/build-and-run/Build.md` has the table).
Docs and workflows show the command that reads the file; they never restate
its contents. External datasets and model weights are pinned by Hub revision
and SHA-256 in `env/assets.json` and fetched by `scripts/provision-assets.py`;
code names a Hub repo only through `scripts/assets_manifest.py`
(`check_docs.py` enforces it). Machine-specific paths go in gitignored
per-machine files (`CMakeUserPresets.json`, `.claude/settings.local.json`),
never in shared config.
