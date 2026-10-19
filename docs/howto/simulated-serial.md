# Simulated serial hardware

Run MIB Studio, or a test, against fake COM ports that behave like the bench:
pumps on an RS485 adapter, the pulse generator, the OEABT nanopositioner and
the ZC300 stage, with real startup timing. Use it to see port races before
they reach the rig.

The unit-test fakes (`tests/support/fake_modbus_bench.h`, `fake_zc300.h`)
answer instantly and are injected into one service. The simulated rig
replaces the OS ports for the **whole process**, so every component that
opens a port (bus sessions, the nanopositioner transport, discovery probes)
meets it through its normal code path. It behaves like hardware in the ways
that cause races:

| Behaviour | Why it matters |
|---|---|
| Exclusive open on every platform (Windows semantics) | Linux lets a second `open()` of a tty succeed silently, so a double-open that fails on the Windows rig passes on Linux CI. The rig refuses it with `EACCES` / `ERROR_ACCESS_DENIED`, which the bus layer reports as `PortBusy`. |
| `presentAfterMs`: late USB enumeration | The first startup attempt misses the device; retries must find it. |
| `openLatencyMs`, jittered `replyLatencyMs` from a seed | Interleavings change per seed; a failing seed replays exactly. |
| `silentAfterOpenMs` | Adapter or MCU boot: the first frames after an open are lost. |
| `fragmentReplies` | Replies arrive in two chunks; readers must reassemble. |
| `unplugged` windows, `Rig::unplug` / `replug` | Cable pulls: I/O on an old handle fails, enumeration drops the port. |
| `staleHolderUntilMs`, `Rig::holdByStaleProcess` | Another process holds the port: an instance that crashed or hung without closing it (#417), or a vendor tool. New opens are refused until it lets go. |
| Wrong baud | The device never answers; recorded as `BaudMismatch`. |

Every open, refusal, close and stray frame goes into a **ledger**
(`Rig::events()`, `Rig::summary()`). A refusal names who held the port and
what it was sending (`held by [thread] for 43ms (ascii '/1&\r')`). A frame no
device on the port understands is a `ForeignFrame`, such as an ASCII probe
written onto the RS485 pump bus. With `MIB_SERIAL_MODE=sim` the ledger is
logged as it happens (`serial-sim:` lines, warnings for anything but
open/close).

## Run the app on it

```bash
MIB_SERIAL_MODE=sim ./build/linux/Release/mib_studio_qt          # built-in bench
MIB_SERIAL_MODE=sim MIB_SERIAL_SIM_SCENARIO=my-rig.json \
MIB_SERIAL_SIM_SEED=7 ./build/linux/Release/mib_studio_qt
```

Combine with `MIB_CAMERA_MODE=mock` for a session with no hardware at all.
The built-in bench (`serialsim::makeBenchRig`):

| Port | Adapter | Devices |
|---|---|---|
| COM3 | CH340 (1a86:7523) RS485, enumerates after 300 ms | dLSP Sample pump addr 1, Sheath pump addr 2, 115200 |
| COM4 | FT232R (0403:6001) | pulse generator addr 1, 9600 |
| COM5 | CH340, enumerates after 1.2 s, silent 150 ms after open | OEABT nanopositioner |
| COM6 | FT231X (0403:6015) | ZC300 Z stage addr 1 (identity registers) |

Point the pump settings at COM3, the generator at COM4 (9600 baud). An
unreadable or invalid scenario stops the backend from starting. It never
falls back to the real ports while you believe they are simulated. The
CoreMorrow vendor DLL opens ports itself, so CoreMorrow endpoints are not
offered under the simulation.

## Scenario file

```json
{
  "seed": 7,
  "exclusiveOpen": true,
  "ports": [
    {
      "name": "COM3", "description": "USB-SERIAL CH340", "vid": "1a86", "pid": "7523",
      "presentAfterMs": 300, "openLatencyMs": 15, "replyLatencyMs": [2, 12],
      "silentAfterOpenMs": 0, "fragmentReplies": false,
      "staleHolderUntilMs": 0,
      "unplugged": [{"fromMs": 20000, "toMs": 25000}],
      "devices": [
        {"type": "dlsp-pump", "address": 1, "baud": 115200},
        {"type": "tushui-pump", "address": 3},
        {"type": "modbus", "address": 9, "baud": 115200, "holding": {"0": 1}, "input": {"5": 77}}
      ]
    },
    {"name": "COM5", "vid": "1a86", "pid": "7523", "devices": [{"type": "oeabt"}]}
  ]
}
```

Device types: `dlsp-pump`, `tushui-pump`, `pulse-generator` (default 9600),
`zc300` (`serial`), `oeabt`, `modbus` (generic register bank; `holding` and
`input` map wire addresses to values). `exclusiveOpen: false` mimics POSIX and
records second opens as `SharedOpen`. `staleHolderUntilMs: -1` holds the port
for the whole session. `unplugged.toMs` omitted means it never comes back.

## Use it in a test

```cpp
#include "backend/services/SimulatedSerial.h"
namespace serialsim = backend::services::serialsim;

auto rig = std::make_shared<serialsim::Rig>(seed);
serialsim::PortSpec bus;
bus.systemName = "COM3";
bus.vendorId = 0x1a86; bus.productId = 0x7523;
rig->addPort(bus);
rig->addDevice("COM3", serialsim::makeDlspPump(1));
serialsim::install(rig);          // before AppBackend::initialize
// ... drive the real AppBackend / services ...
MIB_EXPECT(rig->contentionCount() == 0, "no two owners wanted one port");
serialsim::install(nullptr);
```

Run several seeds and print `rig->summary()` for a failing one. Examples:
`tests/backend/serial_sim_test.cpp` (the rig itself) and
`tests/backend/serial_startup_race_test.cpp` (startup against the operator's
connects, stale holders).

## What it found

On its first run, the startup nanopositioner probe opened a CH340 RS485 pump
adapter (CH340 is an OEABT candidate by USB identity). It held the port for
~2 s and wrote ASCII onto the pump bus. A pump Connect in that window failed
with "port busy", on every seed. Fixed by in-process port claims
(`SerialPortClaims.h`). Probes skip ports a service holds or that config.json
assigns to the pumps. A service waits for a probe on its port instead of
failing. See [the vault note](../../knowledge_map/services/SerialPortClaims.md).
