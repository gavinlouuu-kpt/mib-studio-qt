# Local analysis helper development endpoint

`helper.py` implements an experimental protocol **0.1**, exercised over private
stdin/stdout pipes by `test_helper.py`. It is not launched by Tauri yet and is
not a production runtime. It opens no network listener, dataset, output file,
camera, or trigger. Native Review remains responsible for bounded recording reads.

`crates/mib-analysis` now provides independently tested Rust process supervision:
digest-verified bundle inventory, deadlines, cancellation and Linux parent-death
ownership. When the supervisor passes `--bundle-manifest`, the handshake also
requires `bundle_sha256` and echoes the manifest/wheel identity. `production_ready`
reflects the trusted manifest's distribution mode; unbundled development launch
still reports false. The supervisor validates the bundle before executing it;
a handshake claim alone is not trust. See `crates/mib-analysis/README.md`.

The source dependency is `gavinlouuu-kpt/Biowork-toolkit` at
`388924e5c9d95e0691b969be6238f3e94db817d4` (package `0.1.0`). Use a checkout at
that revision, or a locally built wheel from it, for development. The registry
publisher fix in Toolkit PR #2 changes no scientific package code. A development
wheel is not a production release, and matching a version is not a trust check.

With that checkout's locked development environment available:

```bash
uv run --project /absolute/path/to/Biowork-toolkit --locked --extra dev \
  python -I /absolute/path/to/mib-studio-qt/desktop/analysis/test_helper.py
```

Each frame has a four-byte unsigned big-endian byte length followed by UTF-8
JSON. The limit is 1 MiB in either direction. Duplicate keys, non-finite numbers,
invalid UTF-8, malformed/truncated frames, and unknown envelope fields fail closed.
EOF between frames exits cleanly; invalid requests terminate with exit code 2
and no success reply. The parent must classify an unanswered operation as failed
or unknown; it must never infer successful completion from EOF.

An envelope contains exactly `protocol: {major: 0, minor: 1}`, increasing integer
`request_id` (1 through 2^53-1), `operation_id` (1–64 characters), integer
`generation` (0 through 2^53-1), `method`, and object `params`. Replies echo all
identities with `status: completed` and `result`. IDs never accumulate in a cache.

| Method | Parameters | Limit / semantics |
|---|---|---|
| `handshake` | `toolkit_version: "0.1.0"` | First request only; reports `production_ready: false` |
| `select_generation` | Empty object | Must strictly advance generation |
| `histogram_page` | `values`, `metric`, `bins` | Up to 4,096 finite numbers; 1–256 bins |
| `kde_page` | `x`, `y`, `columns`, `rows`, `bandwidth` | Equal columns up to 4,096 pairs; each grid dimension 2–128; bandwidth 0.5–8 |

Calculations call Toolkit's existing implementations. These are **page results**,
not whole-recording distributions; independently scaled histograms or KDE pages
must not be merged or relabeled as full-dataset analysis. Finite inputs that
overflow scientific calculation/serialization fail closed. No arrays or datasets
remain resident between requests.

The endpoint is deliberately serial. Cancellation during calculation requires
the Rust supervisor to terminate/reap the child independently of this
channel; sending a cancel method behind a calculation is not supported. There is
no pending-request queue or asynchronous publication in this endpoint.

Before enabling the desktop capability, wire the supervisor to the native ledger,
supply an installer-protected runtime with a trusted build pin, add bounded
diagnostic retention, Windows Job Objects and durable restart classification.
Clean-machine installers and NAS
qualification remain required by issue #399. Protocol 1.0 is not frozen by this
development endpoint.
