# 0007. A shipped processing core implements exactly one contract

Date: 2026-09-25
Status: proposed

## Context

[ADR 0006](0006-processing-contract-v2.md) introduced Processing Contract 2 and
required that "a consumer refuses to execute a profile against an
implementation whose contract does not match". The
[compatibility matrix](../architecture/processing-contract-compatibility.md)
encodes that: a Contract-2 profile on a Contract-1 core is refused, and the
native loader already checks `expectedContractVersion`.

V2-8 then made the contract a **runtime setting**
(`ProcessingConfig::processing_contract_version`), so the bundled kernel runs
either contract depending on the profile. That created two numbers that can
disagree, and both defects found on 2026-09-25 come from that disagreement:

- Recordings stamp `processing_contract_version` from the core identity
  (`MIB_PROCESSING_CONTRACT_VERSION` = 1), while the config decides which
  contract actually runs. A Contract-2 recording is labelled Contract 1 and
  exported as Contract 1.
- Native descriptors declare one `contract_version` (1). Nothing stops a
  Contract-2 profile from being run by an older Contract-1-only core.

A runtime switch also means that any work on a new contract rebuilds, and can
change, the binary that rigs use for the old one. The only protection is then
the gold references.

## Decision

1. **One shipped core, one contract.** Every core that runs on a rig
   implements exactly one processing contract, fixed at build time. That covers
   the kernel bundled into a desktop build and each signed native plugin. The
   core's identity and its descriptor `contract_version` declare it.
2. **Choosing an algorithm means choosing a core.** A profile's
   `processing_contract_version` is a **requirement**, not a selector. The
   application processes with a profile only when its contract equals the active
   core's contract. Otherwise it refuses to start processing and names both
   numbers. There is no fallback to another contract.
3. **Core version and contract stay separate.** The contract identifies the
   science: what the numbers mean. The core version identifies the build: bug
   fixes, performance, platform builds, and new non-science outputs such as
   `in_channel`. Each contract has its own core release line and gold references.
4. **Several contracts in one binary is for research only.** The Python wheel
   and tests may select the contract per call, so both contracts can be compared
   on the same frames. Wheel results declare the contract that produced them.
   Wheel output is never a rig's processing path.
5. **Provenance comes from the core.** A recording's contract is the active
   core's contract. Because that core can run only one contract, the recorded
   contract is the executed contract. Recordings also keep the core version, the
   artifact SHA-256, and the processing configuration.
6. **Shared source, separate gold references.** All core lines build from one
   codebase. Each build must reproduce its own contract's frozen gold
   references. A change to shared code rebuilds and re-verifies every maintained
   line, and it may not change any line's reference output (see ADR 0006: such
   changes are bugs).
7. **Core lines are named after their algorithm.** A line's name joins its
   background-difference method and its focus metric: `subtract-ring`
   (Contract 1) and `absdiff-laplacian` (Contract 2). The name appears in
   release tags (`mib-processing-<name>-v<semver>`), artifact file names,
   descriptors, the registry and the UI (e.g. "Absdiff + Laplacian (Contract
   2)"). The contract number stays the machine-checked identifier, and the name
   is its fixed human label. Names are registered once, in the compatibility
   matrix, and never reused. A later contract that shares a step is named by
   what it changes. The published `mib-processing-v0.1.0` and `v0.2.0` releases
   are immutable and remain valid as legacy `subtract-ring` releases.
8. **Rollback means activating the previous core.** On a rig this is done
   through the Processing Core dialog. That is either the previous version of
   the same contract's core, or a Contract-1 core together with a Contract-1
   profile.

## Consequences

- The mismatch defects above reduce to one check: profile contract equals the
  active core contract. It is enforced by the service, the bundled kernel and
  the native plugin, and each of them fails closed.
- Developing a new contract never rebuilds or re-signs the core binary a rig
  uses for an existing one.
- Desktop builds bundle the Contract-1 kernel by default. Contract 2 reaches a
  rig only as a signed Contract-2 plugin core, so ABI v2 loader activation and
  signing (V2-5) is a prerequisite for any rig use of Contract 2.
- Every release signs one artifact per maintained contract, and the registry
  exposes the contract of each artifact.
- `ProcessingConfig::processing_contract_version` stays in the science library.
  The wheel needs it, and it lets the per-contract builds share code. Shipped
  kernels and the service validate it against the core contract instead of
  obeying it.
- The V2-8 section of the compatibility matrix must be rewritten to match.
  Future contracts must follow this ADR: new contract, new core line, new gold
  references.
