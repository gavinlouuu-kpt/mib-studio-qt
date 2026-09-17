# 0006. Dot-grid wafer localization as a Qt-free decoder plus a polling service

Date: 2026-09-17
Status: accepted

## Context

The camera on the sorting setup looks at a 562 × 351 µm patch of a 100 mm
wafer (20x, through the glass side of a bonded PDMS chip) and has no way to
know where on the wafer, or on which of the 20 chips, it is. The channel
geometry alone is ambiguous at that magnification. We want an absolute
position from any single frame, without a stage encoder, without changing
the fabrication flow, and without touching the real-time processing path.

Options considered: sparse coded markers (ArUco-like) — fail at 20x because
a 1 mm marker spacing is larger than the field of view; matching channel
geometry against the CAD — ambiguous on straight segments; an Anoto-style
displaced-dot lattice — works at any magnification as long as ~6 × 6 dots are
visible and needs no extra mask layer.

## Decision

- Encode position as a displaced-dot lattice on the **channel-layer mask**
  (30 µm pitch, 12 µm dots, 5 µm shift for Wafer_soRT), with separable
  column/row m-sequence phase codes and unique 2-symbol delta windows (see
  `docs/architecture/dot-grid-localization.md`). The codebook is a pure
  function of a seed and geometry, generated identically by the Python mask
  tool and the C++ app; a golden test pins both.
- Implement the decoder in the Qt-free `mib_processing` library
  (`backend::dotgrid`) so it is unit-testable on the Linux backend preset
  and available to the Python bindings, with a synthetic renderer beside it
  for tests and mock frames.
- Run it from a dedicated `DotGridService` thread that samples the latest
  committed `FrameStore` frame at a low, configurable rate (250 ms) and
  publishes a value-typed `Pose` through a mutex snapshot plus an optional
  callback — the same pattern as the realtime drop-frames mode and the
  background-capture callback. No new cross-thread mechanism.
- Expose it in the Preview page as an overlay behind a **Wafer Grid** toggle
  and in `config.json` under `dot_grid`, with a `dot_grid` token in
  `MIB_DISABLED_SERVICES`.

## Consequences

- Easier: absolute localization from any frame, chip identification, and a
  measured µm/px and mirror flag for free; mask generation and app decoding
  cannot drift apart because the codebook is regenerated from the seed.
- Harder / to respect: mask and app must share `(seed, pitch, dot, shift,
  origin)`; the decoder reports **mask** coordinates (the design is 1.5 %
  pre-enlarged); the service must stay off the capture/realtime threads and
  keep decoding a frame index at most once; any change to the encoding must
  update both implementations and the golden values.
- Not done yet: persisting poses into HDF5 metadata, a CAD overlay of the
  channel map, stage-driven navigation (see the exec plan).
