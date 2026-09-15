# OEABT Nanopositioner Integration

Status: active

## Goal

Provide a clean-room OEABT serial compatibility layer, diagnostic CLI, and
cross-platform MIB Studio nanopositioner backend while retaining Windows
CoreMOR support and preventing implicit actuator writes during discovery or
connection.

## Acceptance criteria

- [x] Pure C++17 protocol core encodes the recovered commands and validates
      framed responses through an injected byte transport.
- [x] Qt SerialPort adapter and `oeabtctl` support safe discovery/status plus
      explicitly gated voltage write/restore validation.
- [x] Linux MIB Studio uses the OEABT backend; Windows can select OEABT or the
      existing CoreMOR backend independently of EGrabber availability.
- [x] Connecting is observe-only and a read-only session disconnects without
      changing voltage; an active session applies configured safe shutdown.
- [x] Unit, pseudo-transport, configuration-migration, and hardware-gated tests
      cover the new behavior.
- [x] Documentation and vault notes describe protocol evidence, safety gates,
      endpoint persistence, and connected-controller readback limitations.

## Decision log

- 2026-08-31: Implement a user-space driver because Linux already binds the
  WCH CH341 bridge; no kernel module or generic VID/PID udev rule is needed.
- 2026-08-31: Keep the protocol core Qt-free and place QSerialPort behind an
  adapter so the wire implementation is independently testable.
- 2026-08-31: Preserve CoreMOR on Windows, add auto-selection with explicit
  override, and migrate legacy COM settings to CoreMOR.
- 2026-08-31: Require a successful identity response before reads or writes;
  VID/PID only identifies a probe candidate.
- 2026-08-31: Keep voltage writes opt-in and require real-hardware
  set/readback/restore acceptance before normal release enablement.

## Progress

- [x] Inspect installer, payload, serial settings, command construction, and
      current MIB Studio integration.
- [x] Implement and test protocol core.
- [x] Implement Qt transport and CLI.
- [x] Refactor service/backend selection and configuration/UI integration.
- [x] Complete documentation and software verification. Full backend test
      preset passes 77/77; the TSan lane passes 45/45, including the threaded
      PTY backend; four opt-in physical-hardware tests skip normally.

The silent-handshake blocker was resolved by connected-controller tests on
September 10–11 (firmware V0.5.4). The 30 V → 0 V → 30 V sequence acknowledged
all commands, but zero and low-voltage readbacks differ from their targets.
The integration guide retains the exact observations; physical displacement and
voltage-accuracy release acceptance remain open. September 15 regression fixes
cover four-axis capability fields and unframed firmware diagnostic output.
