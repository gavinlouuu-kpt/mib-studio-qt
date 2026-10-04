# Dot-grid codec cores (swappable encoder/decoder)

Status: active

ADR: [0010 — Dot-grid encoder and decoder as versioned, swappable codec cores](../../decisions/0010-dot-grid-codec-cores.md)
Model: the processing cores ([ADR 0006](../../decisions/0006-processing-contract-v2.md),
[ADR 0007](../../decisions/0007-one-contract-per-shipped-core.md),
[hot-swappable processing cores](2026-07-13-hot-swappable-processing-cores.md))
Design: [architecture/dot-grid-localization.md](../../architecture/dot-grid-localization.md) (Codec cores)

## Goal

The dot-grid encoder and decoder are versioned cores that can be updated the
way processing cores are: a decoder improvement reaches a rig as a signed core
without an app rebuild; a new encoding is a new codec contract with its own
core and frozen gold reference; every fabricated mask stays decodable by the
core of its contract; the mask generator and the app run the same core.

## Mapping from the processing cores

| Processing core | Dot-grid codec core |
|---|---|
| `processing_contract_version` (science) | `codec_contract` (meaning of the dots), line `mseq63-delta2` |
| core version (`pyproject.toml`) | `scripts/dot_grid/dotgrid/VERSION` → `MIB_DOTGRID_CORE_VERSION` |
| `IProcessingKernel` + bundled kernel | `ICodec` + `bundledCodec()` |
| one contract per shipped core; profile contract is a requirement | one contract per core; **one active core per contract** (`CodecSet`), each design routed by its `codec_contract` |
| `ProcessingCoreIdentity` in HDF5 | `codecContract` / `coreVersion` / `coreSource` on every result and pose (HDF5: with pose persistence) |
| `ProcessingCoreAbi.h` (`mib_processing_get_api[_v2]`) | `DotGridCoreAbi.h` (`mib_dotgrid_get_api`) — phase 2 |
| loader: sha256, signature, fingerprint, descriptor, self-test, fail closed | same steps, shared code — phase 2 |
| wheel = research build of the core | wheel exposes the codec core; the mask generator calls it — phase 3 |
| catalog `processing-core/` + dialog + promote workflow | catalog `dotgrid-core/`, tags `mib-dotgrid-<line>-v<semver>` — phase 4 |
| gold references + `gold-reference-change` label | `scripts/dot_grid/gold/codec-contract<N>.json` under the same label |

## Acceptance criteria

### Phase 1 — contract, interface, gold (PR #472)
- [x] `codec_contract` required on every registry design; `encoder` provenance
      recorded by `register`; Wafer_soRT back-filled as contract 1.
- [x] `ICodec` (encode + decode), bundled contract-1 core, `CodecSet` (one core
      per contract), `DesignDecoder` routing designs by contract; `Decoder`
      fails closed on other contracts.
- [x] Designs of a contract without a core are listed as unsupported, logged,
      never decoded; the rest of the registry keeps working.
- [x] Results and poses carry contract, core version and source;
      `DotGridService::activeCodecs()`.
- [x] Single-source core version (`VERSION`) for C++ and Python.
- [x] Gold reference `codec-contract1.json` (exact encode + 12 decode cases),
      met by the C++ core (`processing.dot_grid_codec_gold`) and the Python
      reference (`scripts.dot_grid_reference`); guarded by
      `gold-reference-change`.

### Phase 2 — native plugin core
- [ ] `include/backend/processing/DotGridCoreAbi.h`: pure C, `struct_size` on
      every struct, reserved fields, host-owned buffers, status + error buffer
      (same rules as `ProcessingCoreAbi.h`). Entry `mib_dotgrid_get_api(abi,
      host_size, out, err, cap)`; descriptor (abi, contract, core version,
      build id, runtime fingerprint); opaque codebook handle
      (`create_codebook(params)` / `destroy_codebook`, so a future contract
      need not share `Codebook`); `encode_phases` into host buffers (gold);
      `decode(ctx, image_view, codebooks[], config, out_result, dots_buffer)`
      with BUFFER_TOO_SMALL semantics; `self_test`.
- [ ] Plugin target `mib_dotgrid_core_<line>` (MODULE, hidden visibility,
      export audit = exactly one symbol) built from the bundled codec sources.
- [ ] Loader: generalize the processing-core loader steps (sha256, trust
      verifier, Authenticode/Ed25519 pins, runtime fingerprint, descriptor
      match, self-test, content-addressed cache) into a shared signed-module
      helper used by both families; `DynamicCodec` adapter implementing
      `ICodec`; fixture plugins (good / truncated struct / wrong contract /
      null fn) and a fixture-matrix test.
- [ ] A loaded plugin must meet its contract's gold file before activation
      (loader runs the encode gold; CI runs the decode gold on the artifact).

### Phase 3 — one core for mask generation and the app
- [ ] Expose the codec core in the Python wheel (`mib_processing.dotgrid` or a
      separate `mib_dotgrid` wheel, decided with the release line); the mask
      generator's `register`/`mask` use it and record its identity as
      `encoder`.
- [ ] The pure-Python codebook stays only as the independent reference that
      generated the gold file.

### Phase 4 — distribution and rig activation
- [ ] Catalog `<base>/<channel>/dotgrid-core/{index,latest,versions/<v>}.json`
      (processing-core schema, entry `mib_dotgrid_get_api`, `contract` field).
- [ ] Dialog (or a section of the Processing Core dialog) listing active
      codec cores per contract, unsupported designs, install/rollback.
- [ ] Release workflow and tags `mib-dotgrid-<line>-v<semver>`, signing jobs
      reused from `python-wheel.yml`, promote/rollback workflow.
- [ ] Pose persistence in HDF5 records the core identity (with the existing
      HDF5 follow-up of the localization plan).

## Decision log

- 2026-10-02: several contracts active at once (one core each) rather than
  ADR 0007's single active core, because a bench mixes wafer generations.
- 2026-10-02: a design with an unknown contract is unsupported per design,
  not a registry-wide failure, so an older app keeps decoding the rest.
- 2026-10-02: decode gold is judged against rendered truth, not recorded
  decoder output, so implementations with different renderers and better
  decoders can share one reference; encode gold is exact.

## Progress

- [x] 2026-10-02 — Phase 1 in PR #472.
