# Processing-core version registry

**Date:** 2026-07-13

**Issues:** [A6 #238](https://github.com/KPT1020/mib-studio-qt/issues/238),
[A9 #237](https://github.com/KPT1020/mib-studio-qt/issues/237)

**Plan:** `docs/exec-plans/active/2026-07-13-hot-swappable-processing-cores.md`

## Outcome

The original `processing-core/latest.json` remains the compatibility endpoint,
but it is now one view of a version registry. Each release also has an
immutable `versions/<version>.json`; `index.json` provides enumeration and the
channel-active pointer; a generated PEP 503 page provides hash-qualified wheel
links. The page is stored at both `index.html` and the literal trailing-slash
object key pip requests because R2 does not synthesize directory indexes.
Manifest schema v2 adds canonical `version`, wheel size/filename, and
optional signed-native-plugin metadata without removing schema-v1 fields.

`publish-processing-core.py --from-release` uses `gh` to fetch the assets that
were actually attached to a `mib-processing-v*` release. It calculates URLs,
sizes, and hashes rather than trusting sidecars. Before mutation it reads both
the immutable key and current catalog. Conflicting immutable content or an
unreadable catalog stops publication. The immutable document is uploaded
first, index/package metadata next, and `latest.json` last. `latest.json` is
therefore the canonical active pointer; catalog `active_version` is only its
enumerable mirror. Mutating commands require direct S3 reads from R2 so a
cached public catalog can never become the merge base.

Rollback uses `--promote-version`: the publisher validates an existing
immutable version and copies its original bytes to `latest.json` rather than
reconstructing it with current schema/formatting rules.

## Release and version workflow

The wheel workflow now covers Python 3.10–3.13 across `x86_64` and `aarch64`,
with an install/import smoke test for all 8 wheels and the
existing full-parity conformance gate on x86_64. A Windows job builds the native DLL/sidecar and requires
Authenticode signing on release tags. The release job attaches all artifacts,
then publishes to R2 using repository secrets. Missing signing or R2 secrets
fails a tag workflow rather than silently creating an unusable stable version.
Release reruns accept an existing GitHub Release only when its complete,
canonical asset-name set is present. They reuse those immutable release bytes;
fresh wheels and timestamped Authenticode signatures are not assumed to be
byte-reproducible.

`scripts/bump_mib_processing_version.py` atomically updates the authoritative
wheel pyproject and import-time `__version__`. Its optional tag step requires
both files already committed at `HEAD`, avoiding the old failure mode where a
tag could name a version absent from its commit.

## Gotchas

- `latest.json` is full metadata, not a thin redirect. Removing fields breaks
  existing Biowork clients.
- Version manifests use deterministic release `publishedAt` in CI; otherwise
  a retry would differ at an immutable key. Manual timestamps must be
  timezone-aware, and JSON is emitted as UTF-8 with LF newlines on every OS so
  Windows and Linux publishers produce the same immutable bytes.
- Native descriptor URL/hash fields are ignored. The publisher derives those
  from the sibling DLL after signing.
- Catalog order and activation are separate: `latest.json` is canonical, while
  rolling back changes its catalog mirror without pretending the old version
  is newest.
- R2 and production-signing behavior is wired and unit/YAML-verified here but
  remains a live A12 gate.

**Related:** [[../domain/Glossary]] · [[../build-and-run/Build]] ·
[[../current-state/Recent-Work]]
