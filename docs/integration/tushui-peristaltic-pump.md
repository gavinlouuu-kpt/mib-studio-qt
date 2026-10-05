# Tushui peristaltic pump

The PZ7035 instrument (YOFO Studio) drives a Tushui (惠州徒水流体科技)
peristaltic pump. It can fill either the Sample or the Sheath pump slot
instead of a Longer dLSP syringe pump. The driver is the peristaltic path of
`SyringePumpService`; the register map is `include/backend/services/TushuiPumpProtocol.h`.

## Source document

"徒水蠕动泵 Modbus 通信协议 V2.21" (Huizhou Lianhe Zhongwei / Tushui / Mango
Fluid, Excel export dated 2025-07-16, 4 pages). The vendor PDF is not in the
repository. A copy is at
`/mnt/hdd/shared/projects/mib-studio-qt/tushui-pump-20261004/`.
The document covers only the simplified command set (registers 100-107). The
full "通用版本" protocol it mentions (suck-back, timed dosing, faults) is not
in hand.

## Wire protocol

- Modbus RTU, 1 start / 8 data / no parity / 1 stop, default 115200 baud.
- Function codes 03 (read), 06 (write one), 10 (write many). CRC-16 is
  polynomial 0xA001, sent low byte first (the shared `ModbusRtu.h`).
- Addresses 0x01-0xFE; 0x00 and 0xFF are broadcast (executed, never answered).

| Register | Meaning | Scale / values | Access |
|---|---|---|---|
| 100 | Head speed | rpm x100; 0.01-500.00 rpm (model dependent) | R/W |
| 101 | Direction | 0 clockwise, 1 counter-clockwise | R/W |
| 102-103 | Turns per run | x1000, 32 bit high word first; 0 = run until stopped | R/W |
| 104 | Run | write 0 stop / 1 run; reads may also be 2 suck-back, 3 timing, 4 paused | R/W |
| 105-106 | Baud rate | 32 bit; 2400-115200 | R/W |
| 107 | Slave address | 1-254 | R/W |

The vendor's examples (`01 03 00 64 00 01 C5 D5`, `01 06 00 64 04 D2 4A 88`,
`01 06 00 68 00 01 C9 D6`) check out against the shared CRC.

## Observed on the instrument (2026-10-04)

The pump was read from Linux on the PZ7035 PS. The path is PS UART1
(MIO48 TxD / MIO49 RxD) through the SP3485 transceiver to `/dev/ttyPS1`. The
transceiver switches direction by itself, so no RTS control is needed.

- It answers at **slave address 3**, 115200 baud. Ten out of ten reads came
  back clean.
- State as found: 200.00 rpm, counter-clockwise, 1.000 turn, stopped, baud
  register 115200, slave id 3.
- It also answers registers 0-7 and 108-111, which are outside the
  simplified set: 0-7 = `0001 C200 FFFF 0003 0007 0FA0 0001 00A8`,
  108-111 = `0000 9C40 0001 4E20`. These probably belong to the full
  protocol and are not used.
- The motor has not been run under software control yet.
- The ARMv7 build of `SyringePumpService` connected to the pump and read it
  through `hardware.peristaltic_pump` (read-only mode) on the PS.

## Hardware acceptance

`tests/hardware/hw_peristaltic_pump_test.cpp` (CTest `hardware.peristaltic_pump`)
skips unless `MIB_TEST_PERISTALTIC_PORT` is set. By default it only reads.
`MIB_TEST_PERISTALTIC_RUN_MS=3000` turns the head at 20 rpm infuse
(500 µL/min at 25 µL/rev) for 3 s, about one turn. It then stops the pump and
restores the as-found speed and direction. On the PS:

```bash
sudo env MIB_TEST_PERISTALTIC_PORT=/dev/ttyPS1 MIB_TEST_PERISTALTIC_RUN_MS=3000 \
    /tmp/yofo-pump/mib_backend_tests hw_peristaltic_pump_test
```

## End-to-end through the UI

`scripts/yofo/e2e_pump_ui.py` drives the YOFO Studio UI in headless Chromium
against a `yofo-studio-server` on the PS. It runs the Sample slot through these
steps: Peristaltic, connect, 500 µL/min, Infuse, Service mode + arm, Run,
Stop, restore the as-found settings, disconnect. `RUN_SECONDS` 0 skips the
run. A mock-camera server leaves the PL alone:

```bash
# on the PS (from /tmp/yofo-pump: server, dist/, token); systemd-run survives ssh logout
sudo systemd-run --unit=yofo-pump-e2e --setenv=MIB_CAMERA_MODE=mock \
    --setenv=MIB_MOCK_CAMERA_DIR=/tmp/yofo-pump/mock_frames /tmp/yofo-pump/yofo-studio-server \
    --listen 0.0.0.0:8427 --token-file /tmp/yofo-pump/token --dist /tmp/yofo-pump/dist \
    --data-dir /tmp/yofo-pump/data
# on the host
python3 scripts/yofo/e2e_pump_ui.py http://192.168.137.2:8427 "$TOKEN" /tmp/pump-e2e 3
```

On 2026-10-04 the no-motion pass (`RUN_SECONDS` 0) passed on the instrument.
Connect showed 200 rpm Withdraw, and 500 µL/min read back as head 20.00 rpm.
Restore and disconnect left the pump as found. The run step is still to be
done.

## Integration decisions

- **Slots:** the operator chooses the model per slot (Sample or Sheath).
- **Flow:** flow rate = rpm x calibration. The default calibration is
  25 µL/rev (operator: 0.4 rpm = 10 µL/min), to be replaced by a measured
  value: run N turns, weigh or measure the volume, divide by N.
- **No clamping:** rates outside 0.01-500 rpm fail instead of being clamped.
  The fitted model's real maximum is unknown, and 500 rpm is the protocol
  ceiling.
- **Read-only connect:** connect writes nothing.
- **Continuous runs:** start writes turns = 0 first, because the as-found
  1.000 turn setting would stop each run after one revolution.
- **Purge:** runs at 100 rpm. Stop restores the flow speed and direction.
- **Volume:** delivered volume is estimated from speed between polls. The
  pump has no counter in the simplified set.
- **Direction:** Infuse = clockwise. If Infuse withdraws, swap the tubing
  ends in the head.

## Open

- Measured µL/rev calibration for the instrument tubing.
- First software-controlled run on the instrument; confirm the Infuse
  direction.
- The full protocol document (stall/fault status, suck-back).
- Board image: give the YOFO Studio server access to `/dev/ttyPS1`
  (currently `root:dialout 0660`).
