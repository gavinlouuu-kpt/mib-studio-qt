# Vault Maintenance

The vault is only useful if it stays current. **Every agent that modifies
code must update the vault in the same commit/PR as the code change.**
`scripts/check_docs.py` mechanically verifies wikilink integrity; the mapping
below is the policy it cannot check for you.

## When to update

| If you change... | Update the note at... |
|---|---|
| A service under `src/backend/services/<Name>Service.{cpp,h}` | `knowledge_map/services/<Name>Service.md` |
| A frontend tab / controller / dialog under `src/frontend/` | The matching note under `knowledge_map/frontend/` |
| `src/backend/app/AppBackend.{cpp,h}` wiring | [[architecture/AppBackend]] |
| Threading, data flow, or layering | [[architecture/Threading-Model]], [[architecture/Data-Flow]], [[architecture/Overview]] |
| `src/backend/playback/FrameStore.*` | [[data-model/FrameStore]] |
| HDF5 schema / dataset paths (`Hdf5Service.cpp`) | [[data-model/HDF5-Storage]] + [[services/Hdf5Service]] |
| Camera code under `src/backend/camera/` (ICamera, EGrabber, Mock) | The matching note under `knowledge_map/camera/` |
| `CMakeLists.txt`, `conanfile.py`, `CMakePresets.json` | [[build-and-run/Build]], [[build-and-run/Dependencies]], [[build-and-run/Run-Modes]] |
| `env/assets.json`, `scripts/assets_manifest.py`, `scripts/provision-assets.py` | [[build-and-run/Assets]] |
| Conventions / logging patterns | [[conventions/Code-Conventions]], [[conventions/Logging]] |
| Domain vocabulary (new metric, new concept) | [[domain/Glossary]], [[domain/Microscopy-Pipeline]] |
| **Added a new** service / tab / dialog / camera impl | Create the atomic note AND add it to the cluster's `_MOC.md` AND link it from [[README]] and [[Agent-Onboarding]] |
| **Renamed or removed** any of the above | Rename/remove the note AND update every `[[WikiLink]]` that points to it |

## What to update inside a note

- Change the **Responsibility** paragraph if behavior shifted.
- Update **Key APIs / Entry points** when public signatures change.
- Add new gotchas to **Gotchas**; remove fixed ones.
- Touch **Source:** paths if files moved.
- Update the module's `_MOC.md` if a new sibling concept appeared.

## Shipping the change

1. On every non-trivial feature/fix, also append a short dated entry to
   [[current-state/Recent-Work]] (and — if the work was multi-step — create
   `knowledge_map/task/YYYY-MM-DD-<slug>.md`).
2. Before committing, run `python3 scripts/check_docs.py` — it fails on broken
   wikilinks and broken doc links.
3. If you find any note that disagrees with current code, fix it while you're
   there — the vault is a living document, not an archive.

Do not skip this step. If a reviewer sees code changes without matching vault
updates, they should push back.
