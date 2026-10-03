# 0010. Dot-grid encoder and decoder as versioned, swappable codec cores

Date: 2026-10-02
Status: accepted (phase 1 implemented; phases 2-4 planned)

## Context

ADRs 0008 and 0009 give the dot grid one encoding and one decoder, built into
the app, plus a parallel Python implementation for the mask generator. Two
problems follow as soon as designs accumulate:

- **Fabricated masks are permanent.** A wafer made today must decode in ten
  years, even after the encoding has improved (longer windows, error
  correction, other geometry). Nothing records which encoding a mask used.
- **Decoders improve on their own schedule.** Better dot detection or a faster
  vote should reach a rig without rebuilding the app and without any risk of
  changing what an existing mask means.

The processing pipeline solved the same problem with swappable cores
(ADR 0006: contract versions; ADR 0007: one contract per shipped core; a
pure-C plugin ABI, a signed loader that fails closed, a release catalog, and
frozen gold references under a PR label). The dot grid should follow that
design and differ from it only where a wafer bench differs from a
processing profile.

## Decision

1. **Codec contract vs core version.** The *codec contract* is the meaning of
   the dots: m-sequence, delta windows, seed hashing, bit/displacement
   mapping, lattice semantics. It is frozen forever. Contract 1 is today's
   scheme, line name `mseq63-delta2`. The *core version* is a build of one
   contract's encoder + decoder (`scripts/dot_grid/dotgrid/VERSION`, compiled
   in as `MIB_DOTGRID_CORE_VERSION`). It may improve decode robustness and
   speed and fix bugs, but it may not change its contract's encode output.
   Line names are registered once (`codecLineName`, `CODEC_LINES`) and never
   reused.
2. **Every design declares its contract.** `registry.json` entries require
   `codec_contract` and record the `encoder` core (version, contract, line,
   source) that produced the mask.
3. **One core, one contract; one active core per contract.** As in ADR 0007,
   a core implements exactly one contract. Unlike processing, a bench holds
   wafers of several contracts at once, so the app keeps a `CodecSet` with at
   most one active core per contract, and `DesignDecoder` sends each design
   only to the core of its contract. Exactly one design may decode a frame.
4. **Fail closed per design.** A design whose contract no active core serves
   is listed as unsupported (logged, still counted for id/seed uniqueness),
   never decoded by another core, and never turns the whole registry off: an
   older app keeps decoding the designs it can.
5. **Encode and decode live in one core.** The mask generator and the app must
   use the same core of a contract. Until phase 3, the Python reference core
   and the C++ core are kept identical by the gold reference.
6. **Frozen gold references per contract.**
   `scripts/dot_grid/gold/codec-contract<N>.json` holds exact encode
   references (SHA-256 of the full phase arrays for several seeds, heads,
   probes) and behavioural decode references (synthetic views judged against
   the rendered truth; `decode` cases must decode within tolerance, `reject`
   cases never). Every core of the contract must meet it: CTest
   `processing.dot_grid_codec_gold` (C++) and `dotgrid_cli.py gold` /
   `scripts.dot_grid_reference` (Python). The file changes only in a PR
   labelled `gold-reference-change` (gold-reference-guard.yml).
7. **Provenance.** Every decode result and pose names the contract, core
   version and source (bundled/plugin) that produced it, so a recorded pose can
   be traced to the core that made it.

Phases 2-4 (C ABI plugin + signed loader, the wheel as the encoder, catalog
and release line) follow the processing-core design. They are planned in
[the exec plan](../exec-plans/active/2026-10-02-dot-grid-codec-cores.md).

## Consequences

- Easier: a new encoding is a new contract with its own core and gold file,
  never an edit of an existing one; a decoder improvement ships as a new core
  version that must pass the frozen gold reference; old wafers stay decodable.
- To respect: never change the encode output of a contract (the gold file
  catches it); a new contract needs a new line name, a new gold file and its
  own core; `codec_contract` is required in every registry entry; a decoder
  must not decode designs of another contract (`Decoder` skips them, and
  `DesignDecoder` routes by contract).
- Cost: a second contract means a second decode pass per frame for the
  designs of that contract (as with a second dot geometry today).
- Deliberate difference from ADR 0007: several contracts active at once,
  because a registry can mix wafer generations where a processing profile
  cannot.
