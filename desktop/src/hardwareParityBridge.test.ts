// Bridge ABI 45 (#868 HW-004/026/049, HW-023, HW-033): the serial port list and the pump port
// reservation reach the transport under the commands' names with the arguments the Rust side takes
// (`ports` as a JSON array of strings), and the autofocus status keeps the new members.
import { beforeEach, expect, it, vi } from "vitest";
import { bridge, type AutofocusStatus, type SerialPortInfo } from "./bridge";

const { invoke } = vi.hoisted(() => ({ invoke: vi.fn() }));
vi.mock("@tauri-apps/api/core", () => ({ invoke }));
beforeEach(() => { invoke.mockReset(); });

it("lists the serial ports as the backend reports them", async () => {
  const ports: SerialPortInfo[] = [
    { system_path: "COM3", com_port: 3, description: "USB-SERIAL CH340", serial_number: "", vid_pid: "1a86:7523", simulated: true },
    { system_path: "/dev/ttyUSB0", com_port: -1, description: "FT232R USB UART", serial_number: "A50285BI", vid_pid: "0403:6001", simulated: false },
  ];
  invoke.mockResolvedValue(ports);
  await expect(bridge.listSerialPorts()).resolves.toEqual(ports);
  expect(invoke.mock.calls[invoke.mock.calls.length - 1][0]).toBe("list_serial_ports");
});

it("reserves the remembered pump ports, replacing or clearing the previous list", async () => {
  invoke.mockResolvedValue({ transport_version: 1, ok: true, command: 10, message: "Reserved 2 pump port(s)", operation_id: "0" });
  const result = await bridge.reservePumpPorts(["COM5", "/dev/ttyUSB0"]);
  expect(result.ok).toBe(true);
  expect(result.message).toBe("Reserved 2 pump port(s)");
  expect(invoke).toHaveBeenLastCalledWith("reserve_pump_ports", { ports: ["COM5", "/dev/ttyUSB0"] });
  await bridge.reservePumpPorts([]);
  expect(invoke).toHaveBeenLastCalledWith("reserve_pump_ports", { ports: [] });
});

it("carries the autofocus status message and sequence, optional for an older backend", async () => {
  const status: AutofocusStatus = {
    valid: true, connected: true, enabled: false, current_voltage: 1.5, com_port: -1, average_ring_ratio: 0,
    median_ring_ratio: 0, last_ring_ratio_update_us: 0, ring_ratio_age_us: 0,
    status_message: "Voltage change failed: write timed out", status_sequence: 12,
  };
  invoke.mockResolvedValue(status);
  await expect(bridge.fetchAutofocusStatus()).resolves.toEqual(status);
  const { status_message: _message, status_sequence: _sequence, ...older } = status;
  invoke.mockResolvedValue(older);
  const decoded: AutofocusStatus = await bridge.fetchAutofocusStatus();
  expect(decoded.status_message).toBeUndefined();
});
