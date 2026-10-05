# Zolix ZC300 Controller + TBZF6-60 Z Stage

MIB Studio will drive the Zolix (北京卓立汉光) ZC300 stepper controller as a
user-space Modbus RTU device over the shared [SerialBus](../../knowledge_map/services/SerialBus.md)
layer. The ZC300-1A's FTDI FT232R USB bridge is bound by Linux's `ftdi_sio`
kernel driver; no vendor driver is installed or required. Design and rollout:
[ADR 0013](../decisions/0013-motion-stage-device-class.md) and the
[execution plan](../exec-plans/active/2026-09-30-zc300-z-stage.md).

## Interoperability evidence

All protocol facts below come from the vendor Modbus manual and were
confirmed against a connected controller on 2026-09-30. The vendor Windows
application (NI LabVIEW runtime installer) was not run or inspected.

Retained with the hardware evidence at
`/mnt/hdd/shared/projects/mib-studio-qt/zc300-z-stage-20260930/` (`SHA256SUMS`
alongside):

| File | SHA-256 |
|---|---|
| `ZC300-ModbusRTU通信协议-用户手册-V1.15.pdf` (protocol, 2022-09-19) | `cbd8bea306532d9f5aefd050124ccb6817911a38f1971cd830730b2d4f61cefa` |
| `ZC300系列运动控制器.pdf` (hardware / front-panel manual) | `0c7fc87d2cbe3268b9afb28e3c1fa4e309c90b419be5fdf9ac5c437360b0f1b4` |
| `ZC300控制器软件使用说明书.pdf` (vendor PC software) | `885440ee3ffbb02f32b3ea5ba5772282feb7fd495dbdcee99cbc84fd44ea046c` |
| `TBZF-2023.pdf` (stage datasheet, zolix.com.cn) | `edcb10942d0412d2ce5906a3ef23df80b1550988291bc821a3c324b42a3c33b2` |
| `zc300.py`, `motion_test.py` (pyserial bench client used for the observations) | see `SHA256SUMS` |

## Unit under test

| Item | Value |
|---|---|
| Controller | ZC300-1A (single axis, USB only; `-RI` variants add RS485 + I/O) |
| Model register (30001..7) | `ZC300-1A` |
| Device serial (30008..9) | 26017 |
| Firmware (30010) | 1.2 |
| USB bridge | FTDI FT232R, VID:PID `0403:6001`, iSerial `A10RB8XC` |
| Internal driver | SR2-plus, factory 1.6 A, 1600 pulses/rev (DIP switches, not software-visible) |
| Stage | TBZF6-60 wedge lift stage on axis X |

The `-1A` has one populated axis (X). Y/Z registers still answer; their
home-switch bits read active because the inputs float.

## Wire protocol

- Modbus RTU, 115200 baud (fixed), 8N1, no flow control, slave address 1
  (settable 1–255, register 30069).
- Function codes: `0x03` read holding, `0x04` read input, `0x06` write single,
  `0x10` write multiple. **Motion opcodes must use `0x10`.**
- Manual register numbers are 1-based: wire address = number − 1
  (30050 → `0x7561`).
- 30001–30049 are **input registers (FC 04 only)**; 30050–30140 are holding
  registers (FC 03/06/10). Reading an input register with FC 03 (or the
  reverse) returns exception 02.
- 32-bit values (`float`, `LONG`) are big-endian, high word first — the same
  ABCD order `modbus::floatToRegisters` already implements.

### Registers used by the driver (X axis; Y/Z follow at stride 1 or 2)

| Register | Access | Type | Meaning |
|---|---|---|---|
| 30001..7 | R (FC04) | ASCII | Model string |
| 30008..9 | R (FC04) | LONG | Device serial number |
| 30010 | R (FC04) | SHORT | Firmware ×10 |
| 30012 | R (FC04) | SHORT | X moving (1) / stopped (0) |
| 30015 | R (FC04) | SHORT | bit0 X+ limit, bit1 X− limit, bit2 X home, bit9 e-stop, bit10 X driver alarm |
| 30016..17 | R (FC04) | float | X position, in the axis unit |
| 30022 | R (FC04) | SHORT | X unit (0 pp, 1 mm, 2 deg) |
| 30050..53 | RW | SHORT | Opcode, param 1 (axis), param 2 (direction), param 3 |
| 30059..60 | RW | float | X position (writable: redefines the coordinate, no motion) |
| 30066 | RW | SHORT | X enable (volatile) |
| 30072 | RW | SHORT | X unit |
| 30075..76 | RW | LONG | X pulses per revolution (must equal the driver DIP setting) |
| 30081 | RW | SHORT | X stage type (0 linear, 1 rotary) |
| 30084..85 | RW | float | X lead, mm/rev |
| 30099..100 | RW | float | X home speed |
| 30105 | RW | SHORT | X home mode (1 user, 2 negative limit, 3 home switch) |
| 30114..115 | RW | float | X step distance — **also the absolute-move target** |
| 30120 | RW | SHORT | X stop mode (0 immediate, 1 decelerate) |
| 30123..130 | RW | float | X initial / cruise speed |
| 30135..136 | RW | float | X acceleration |

Speed, acceleration and distance registers are stored in pulses and presented
in the current axis unit, so changing the unit or lead re-scales their
readback without changing motion.

### Opcodes (write to 30050 with FC 10)

| Opcode | Params | Action |
|---|---|---|
| `0x64` | axis, `P`/`N` | Absolute move to ±(step-distance register) |
| `0x65` | axis, `P`/`N` | Relative move by the step-distance register |
| `0x66` | axis, `P`/`N` | Continuous move until stop or limit |
| `0x67` | axis | Decelerating stop |
| `0x68` | axis or `0x30` (all) | Immediate stop |
| `0x69` | axis | Home per the axis home mode |
| `0x6C` | — | Factory reset |
| `0x6D` | — | Save parameters to flash (all axes must be stopped) |

Axis codes: `0x31` X, `0x32` Y, `0x33` Z, `0x30` all. Directions: `P` = `0x50`,
`N` = `0x4E`.

### Exception codes

| Code | Meaning |
|---|---|
| 01 / 02 / 03 | Illegal function / register address / data length |
| 04 | Save failed (an axis was moving) |
| 05 | Illegal data |
| 06 | Motion refused: axis already moving |
| 07 | Motion refused: limit switch active |
| 08 | Motion refused: emergency stop active |
| 09 | Motion refused: axis not enabled |
| 0A | Unknown opcode |

## Observed behaviour (2026-09-30, firmware 1.2)

These are the facts a driver must encode; each maps to a fake-bench test in
the execution plan.

1. **Dropped request after a move.** Immediately after a move finishes, the
   controller occasionally ignores one request (silence, no partial frame).
   300 back-to-back idle reads had zero drops. Reads and idempotent writes may
   be retried; a motion opcode must never be re-sent blindly.
2. **Save acknowledges after ~1.06 s.** Opcode `0x6D` needs a ≥3 s response
   timeout; the default 0.5–1 s times out even though the save succeeds.
3. **Unit-mode distance quantization.** With unit = mm, the controller
   truncates a commanded distance to 0.001 mm before converting to pulses and
   then rounds to the nearest pulse. Requests of 10.0–10.999 pulses
   (4.375–4.81 µm) all moved 9 pulses; 0.1 mm moved 229 pulses. Relative
   returns therefore drift by a pulse; float32 representation error can also
   drop a µm (send `round(d, 3) + 1e-5`).
4. **Absolute moves are exact to the pulse.** `0x64` with the target in the
   step-distance register and `P`/`N` giving its sign returned to 0.000000
   every time; `N` with 0.02 mm reached −0.020 mm.
5. **Position is a counter, not a measurement.** The ZC300 has no encoder
   input; readback is commanded pulses. After a stall, collision or power
   cycle it is meaningless until re-homed. It powered up at 0.
6. **Motion ran with factory settings** (1600 pp/rev, 8000 pp/s, 12000 pp/s²).
   +400 pp took 0.40 s; continuous run → stop after 0.25 s travelled 484 pp.

No homing, limit-switch or e-stop behaviour has been exercised yet.

## TBZF6-60 stage parameters

From `TBZF-2023.pdf` (the ZC300 manual's appendix lists only TBZF10-120 and
TBZF30-200):

| Parameter | Value |
|---|---|
| Travel | 6 mm |
| Screw | ball screw Ø6, 1 mm lead, through a wedge |
| Resolution | 3.5 µm/full step, 1.75 µm/half step, 0.175 µm at 20 µsteps |
| **Effective lead** | **0.7 mm per motor revolution** (200 × 3.5 µm) |
| Max speed | 7 mm/s (600 rpm) |
| Repeatability / backlash / flatness | ≤ 2.5 µm / ≤ 5 µm / ≤ 0.1 mm |
| Rated load | 4 kg (self weight 0.8 kg) |
| Motor | 28-frame 2-phase, STP-28D1003-08, 1.8°, **1.3 A**, holding 78.5 mN·m |
| Sensors | 2 limit + 1 home, PM-L25 photo-interrupters, NPN open collector, DC 5–24 V |
| Connector | DB9 male, 0.2 m flying lead |

The effective-lead derivation reproduces the ZC300 manual's published values
for the other two TBZF models (0.364 and 1.298 mm/rev).

At 1600 pp/rev one pulse is **0.4375 µm**. Controller saved to flash on
2026-09-30: unit mm, lead 0.7, linear; speed 3.5 mm/s, acceleration
5.25 mm/s², home speed 0.35 mm/s, step distance 3.5 mm. Factory values were
unit pp, lead 4, ratio 180, speed 8000 pp/s, acceleration 12000 pp/s², step
8000 pp.

Open hardware items: the driver is set to 1.6 A for a 1.3 A motor (at the edge
of the manual's ±0.3 A guidance; reduce with the SR2-plus DIP switches, power
off); limit/home sensor wiring and polarity are unverified.
