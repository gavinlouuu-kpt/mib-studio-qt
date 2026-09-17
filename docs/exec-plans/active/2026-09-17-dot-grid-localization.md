# Dot-grid wafer localization

Status: active

ADR: [0006 — Dot-grid wafer localization](../../decisions/0006-dot-grid-localization.md)
Design: [architecture/dot-grid-localization.md](../../architecture/dot-grid-localization.md)
Task record: `knowledge_map/task/2026-09-17-dot-grid-localization.md`

## Goal

Know where the camera is on the wafer (and on which chip) from a single
frame at 20x through the glass side, using a dot-grid fiducial pattern
fabricated in the channel layer of the Wafer_soRT design, with the decoding
and overlay integrated in MIB Studio.

## Acceptance criteria

- [x] Pattern encoding defined, parameters chosen for Wafer_soRT
      (30 µm pitch / 12 µm dots / 5 µm shift / 50 µm channel keep-out) and
      validated on synthetic 20x/10x/4x views including the mid-channel case.
- [x] Python mask generator produces `codebook.json` + GDS layer from the
      design DXF; codebook is shared with the app.
- [x] Qt-free C++ decoder in `mib_processing`, identical codebook generation
      (golden test), synthetic renderer.
- [x] `DotGridService` polling FrameStore at a low rate, config under
      `dot_grid`, `MIB_DISABLED_SERVICES=dot_grid`, wired in `AppBackend`.
- [x] Preview page overlay + **Wafer Grid** toggle.
- [x] Tests: `processing.dot_grid_codebook`, `processing.dot_grid_decoder`,
      `backend.dot_grid_service`, `scripts.dot_grid_reference`.
- [ ] Mask fabricated (chrome), test wafer moulded, real-frame decode
      confirmed at 20x mid-channel; tune blob threshold / keep-out from data.
- [ ] Pose persisted per frame in HDF5 (new compound dataset, see
      `Hdf5Service` metadata pattern) when recording.
- [ ] CAD channel overlay from the decoded pose; chip-relative coordinates.

## Decision log

- 2026-09-17: 50 µm pitch rejected — a 20x view centred on a channel with a
  50 µm keep-out never contains enough dots on one side; 30 µm pitch passes
  (28–29/30 synthetic). Lookup window shortened to 2 delta symbols (3
  lines) for the same reason.
- 2026-09-17: dots stay on the channel layer (no second mask); fallback is a
  thin second SU-8 layer if bonding near pits is a problem.
- 2026-09-17: decoder verifies every dot against the codebook (≥ 90 %
  agreement) after decoding; lattice-index drift on large 4x grids otherwise
  produced positions off by a pitch.

## Progress

- [x] 2026-09-17 — Design, Python reference + generator, C++ port, service,
      UI, tests, docs (this PR).
- [ ] Order mask; generate mock-frame set from the shipped codebook for the
      bench PC.
