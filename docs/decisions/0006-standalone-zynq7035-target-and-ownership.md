# ADR 0006 — Standalone Zynq-7035 instrument: target, ownership and boundaries

- Status: accepted
- Date: 2026-09-23
- Issue: #443 (E0 of epic #441; related #445, #446, #447, #448, #449, #423)
- Plan: [2026-09-23-embedded-e0-target-contract](../exec-plans/active/2026-09-23-embedded-e0-target-contract.md)
- Manifest: [`deploy/embedded/pz7035-target.json`](../../deploy/embedded/pz7035-target.json)
  (schema [`pz7035-target.schema.json`](../../deploy/embedded/pz7035-target.schema.json),
  validator `scripts/check_embedded_target.py`, CTest `scripts.embedded_target_manifest`)

## Context

Epic #441 prepares MIB Studio for a self-contained instrument: power on a
Zynq-7035 board, run an experiment from its touchscreen, stop, inspect and
export without a PC, browser server, Internet or cloud. The codebase already
has the pieces the appliance needs — a Qt-free `mib_backend` behind
`AppBackend`/`BackendFacade`, a readiness transaction and run lifecycle owned
by `ExperimentCoordinator`, reconciled accounting, byte-budgeted memory
owners, provider-based discovery (ADR 0005) and a signed processing-core ABI
— but nothing records which board it targets, who owns what between the ARM
processing system (PS), the programmable logic (PL) and the UI, what the
numeric limits are, or which earlier FPGA evidence transfers.

Two earlier FPGA efforts pull in different directions and must not be
mistaken for XC7Z035 evidence: the Ultra96 direct-DDR pipeline (Cortex-A53,
XRT/ZOCL, MPSoC addresses; `docs/exec-plans/completed/2026-07-15-ultra96-direct-ddr.md`)
and the KU5P/MicroBlaze IMX426 development platform. The board work now
lives in a second repository, `gavinlouuu-kpt/pz7035-imx426`, whose pinned
state (2026-09-22) has verified IMX426 acquisition over Ethernet from a
bare-metal OCM firmware and short W8A8 U-Net bursts from SRAM — separate
images, no DDR, no Linux.

## Decision

1. **One target, recorded once.** The appliance targets the Puzhi PZ7035-FH
   with the XC7Z035-2FFG900I: dual Cortex-A9, ARMv7-A, 32-bit,
   `arm-linux-gnueabihf`. ARM Linux is the proposed runtime baseline; the
   board's bare-metal OCM firmware is a bring-up diagnostic, not the
   application host, and bare-metal/RTOS portability of the service graph
   is not claimed. AArch64, ZynqMP/Ultra96, KU5P/MicroBlaze and GPU
   assumptions are *excluded targets*: the manifest validator rejects them
   anywhere inside the target description or dependency allowlists. Every
   hardware fact in the manifest carries an evidence class (source review,
   compilation, emulation, simulation, synthesis, target software test,
   physical acceptance, vendor documentation, unknown); unknown values are
   `null` with a named owner, never invented.

2. **Two repositories, one pinned dependency direction.** This repository
   owns the shared application contracts, CPU reference science, runtime and
   experiment authority, recording/replay, hardware-provider interfaces, the
   desktop shell and the ARM headless application targets.
   `pz7035-imx426` owns PL/sensor RTL/HLS, the generated PS–PL interface
   definitions, BSP/kernel/device tree/DMA driver, board-specific provider
   implementations and board image assembly. The board build consumes a
   pinned MIB commit or release; shared code never includes board headers or
   register addresses. There is no MIB fork, copied science tree or third
   "core" repository, and no second "extract a Qt-free core" project — the
   existing `mib_processing`/`mib_backend` boundary is the core.

3. **One owner per operation, three tiers.** The manifest's ownership matrix
   assigns every standalone-required operation to exactly one of PL, PS or
   UI with its data, rate/deadline, failure policy and test:
   - **PL** owns sensor ingress, the accepted high-rate science stages,
     physical trigger/sort scheduling, and output inhibition/watchdog that
     must survive PS, OS or process failure. No automatic re-arm after a
     crash, reboot or update.
   - **PS (the C++ runtime)** owns configuration validation, readiness, the
     run lifecycle, device orchestration, bounded result ingestion,
     persistence/finalization/accounting, diagnostics and the client-loss
     policy. Exactly one hardware-owning backend instance exists; desktop
     callers stay in-process, the appliance UI talks to it over E2's local
     API.
   - **UI** owns input, editor drafts, status and bounded preview/result
     presentation only. UI visibility, navigation, restart or lost input can
     never own acquisition, finalization, readiness or safety.
   Tracking/target/gating is recorded with PS as the *provisional* owner and
   status `unresolved`: it is admitted on ARM only after its A9 cost is
   measured against the sensor-to-output deadline; otherwise it moves to PL.
   A missing PL capability is a readiness failure, never an unqualified
   software fallback.

4. **Two execution paths, three independent protocol boundaries.** The host
   SDK full-frame path (`CaptureService` → `FrameStore` → `ProcessingService`)
   and the FPGA result-first path (E3's execution-provider adapter) remain
   distinct and share only the lifecycle/config/result contracts that are
   semantically valid for both; nobody synthesizes host frames from FPGA
   results. The in-process native plugin ABI
   (`MIB_PROCESSING_ENGINE_ABI_VERSION`), the local application IPC (E2,
   reusing the bridge contract's semantics) and the PS–PL wire protocol
   (board-owned, generated) are versioned independently; pointer-bearing
   native structs are never copied into an IPC or FPGA wire format.

5. **Budgets are numbers with a status.** Every limit in the manifest is
   `frozen`, `proposed`, `sizing-input` or `unknown`, with a derivation and
   an overflow policy. Sizing inputs (e.g. 512×96 Mono8 at 5,000 fps =
   245,760,000 raw bytes/s) are not achieved-performance claims. Overflow
   policy is declared separately for required scientific records, optional
   crops and display replacements; full raw recording needs its own measured
   admission gate and is not promised.

6. **The no-UI fixture is the baseline regression.**
   `integration.e2e_headless_experiment_smoke` drives the real backend through
   the facade with an explicit mock source — readiness fails closed, a stale
   generation is refused, Start freezes the run, ≥N frames are admitted, Stop
   finalizes, and the HDF5 file reopens with matching provenance, reconciled
   accounting and a `Complete` outcome — with no Qt, display or network. E1–E5
   build on this fixture rather than adding another lifecycle harness.

## Consequences

- E1 (ARMv7 build), E2 (headless host/API/CLI), E3 (FPGA adapter fakes) and
  E4 (API-backed UI simulator) can proceed in parallel against the manifest;
  only their on-target acceptance waits for the board unknowns (U1–U17).
- Any change to the target, an ownership row, a budget or a boundary
  version is a reviewed edit to the manifest; the validator fails on missing
  evidence, unreferenced unknowns, unregistered test names, excluded-target
  leakage or forbidden appliance dependencies.
- Reports must keep the evidence classes apart. Missing board evidence
  leaves the relevant gate open; an x86_64 backend build is not ARM evidence,
  and Ultra96/KU5P results are not XC7Z035 evidence.
- The desktop (Windows/Linux) behaviour, historical Contract-1 science and
  HDF5 compatibility are unchanged by this decision; #423 remains the
  scientific/FPGA-feasibility gate and this ADR does not select a pipeline.
- Known gaps recorded for their owners rather than fixed here: no
  exact-count admission terminator in the run lifecycle (E2), no
  `linux_armv7` native-core artifact naming (E1), the controller-loss
  policy (E2), and every hardware unknown in the manifest (board PZ1–PZ4).
