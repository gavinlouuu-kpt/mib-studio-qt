# Qualification handoff — not module acceptance

Status: blocked at shared production-contract agreement; first audit/qualification
deliverable complete. All F01–F16 remain open.

## Delivered

- [Ledger](capability-ledger.md): 16 dispositions, research/boards, existing code,
  tests/PRs, owned #523/#314 packages and remaining acceptance.
- [Catalogue](evidence-catalogue.json): ten research hashes, original board IDs and
  flows, live page revision 23/version 0, evidence limits.
- [Contracts](contracts.md) and [bounded plan](../../exec-plans/active/2026-10-22-flowjo-realignment.md):
  logical record proposals, existing authority reuse, #801–#805 reconciliation.
- [Reproducible qualification harnesses](../../../tools/flowjo_acceptance/README.md).

## Observed validation (2026-10-22)

| Check | Result | Evidence class |
|---|---|---|
| Read-only hash-pinned FCS/histogram reference | PASS: 229,585 input; 190,705 selected; 2 × 4,096 bins, zero differences | Historical v10.8.1 artifacts recomputed, not fresh vendor execution |
| Missing reference | PASS: verifier exits 1 | Failure-path check |
| Changed reference | PASS: verifier exits 1 on SHA mismatch | Failure-path check |
| Oversized reference | PASS: verifier exits 1 before decoding | Failure-path check |
| Real Cytoflow 1.3.2 synthetic gate/key probe | **8/10; exits 1** | Real engine, synthetic fixture; not MIB adapter |
| `python3 scripts/check_docs.py` | PASS | Repository documentation gate |
| `python3 scripts/gen_bridge_contract.py --check` | PASS | Existing generated ABI unchanged |
| Python compilation and `git diff --check` | PASS | Harness syntax/whitespace |

Raw sanitized results: [reference](qualification-reference.json),
[Cytoflow](qualification-cytoflow.json). The runtime constraints are a development
record, not a shipping dependency lock. Cytoflow's pin and fcsparser submodule
were checked out exactly; no proprietary implementation was copied.

## Material findings retained

1. Equal inclusive scalar bounds are required by MIB. Bare Cytoflow RangeOp
   refuses `low == high`. Preserve MIB's authoritative scalar semantics in the
   coordinated adapter, with regression tests; do not weaken the requirement.
2. Cytoflow's boolean polygon mask excludes NaNs, but naïvely negating it admits
   those rows. MIB composition must retain evaluability and implement declared
   three-valued logic. This harness demonstrates the hazard; it does not fix it.
3. Default source installation can omit the optional Logicle extension; SWIG
   plus the pinned fcsparser submodule were necessary for this headless probe.
4. Source built/imported does not mean distribution approved, advanced methods
   qualified, supervised jobs implemented or application integration proved.

Primary self-review checked read-only source handling, bounded input reads,
failure exit codes, exact string IDs, absent-value behavior and evidence wording.
No independent code review or full backend/frontend suite is claimed. There
are no production code changes to validate in those suites in this slice.

## Exact pending owners / resume conditions

- #806 producer owner: acknowledge/amend source generation, committed snapshot
  and image resolver seams; qualify the live provider without violating #722.
- #807 experiment owner: acknowledge/amend scientific result/comparison and
  experimental-unit/finding/recipe consumption. No duplicate evaluator.
- #512/#717 Review owner: agree concrete session/bridge/job file ownership and
  #803/#804 sequencing before F implements shared production changes.
- #808/#523 implementation: after agreement, implement source/workspace/worker
  adapter, including the two demonstrated semantics gaps and complete F-A–F-F.
- Product/release owner: decide the pinned Cytoflow distribution plan before
  shipping. Internal qualification remains permitted and was performed.

Coordination requests were posted to #808 and peers #806/#807/#717. No response
or agreement is inferred from posting. No final composition, merge, deployment,
acquisition actuation or change to other running sessions was performed.
The two-recording module demo is **not yet available**; these harnesses are not
its substitute. Resume requires recorded peer agreement, then production work
and independent review—not merely changing this document's status.
