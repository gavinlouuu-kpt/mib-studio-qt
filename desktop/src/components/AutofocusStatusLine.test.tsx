// @vitest-environment jsdom
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it } from "vitest";
import type { AutofocusStatus } from "../bridge";
import { AutofocusStatusLine, isAutofocusFailureMessage } from "./AutofocusStatusLine";

let host: HTMLDivElement, root: Root;
beforeEach(() => {
  Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });

const status = (patch: Partial<AutofocusStatus>): AutofocusStatus => ({
  valid: true, connected: false, enabled: false, current_voltage: 0, com_port: -1,
  average_ring_ratio: 0, median_ring_ratio: 0, last_ring_ratio_update_us: 0, ring_ratio_age_us: 0, ...patch,
});
const render = (value: AutofocusStatus | null) => act(async () => root.render(<AutofocusStatusLine status={value} />));

describe("autofocus status line (HW-033)", () => {
  it("shows the latest message as 'Autofocus status: <message>'", async () => {
    await render(status({ status_message: "Connected to oeabt nanopositioner on /dev/ttyUSB0", status_sequence: 3 }));
    const line = host.querySelector(".autofocus-status-line")!;
    expect(line.textContent).toBe("Autofocus status: Connected to oeabt nanopositioner on /dev/ttyUSB0");
    expect(line.getAttribute("role")).toBe("status");
    expect(line.getAttribute("data-sequence")).toBe("3");
  });

  it("renders nothing before the service has spoken, without a status, or from an older backend", async () => {
    await render(null);
    expect(host.innerHTML).toBe("");
    await render(status({ status_message: "", status_sequence: 0 }));
    expect(host.innerHTML).toBe("");
    await render(status({})); // pre-ABI-45: the members are absent
    expect(host.innerHTML).toBe("");
    await render(status({ status_message: "   " }));
    expect(host.innerHTML).toBe("");
  });

  it("follows a newer message and announces a failure as an alert", async () => {
    await render(status({ status_message: "Voltage: 12.500000V", status_sequence: 5 }));
    expect(host.querySelector(".autofocus-status-line")!.getAttribute("role")).toBe("status");
    await render(status({ status_message: "Voltage change failed: injected write failure", status_sequence: 6 }));
    const line = host.querySelector(".autofocus-status-line")!;
    expect(line.textContent).toBe("Autofocus status: Voltage change failed: injected write failure");
    expect(line.getAttribute("role")).toBe("alert");
    expect(host.querySelectorAll(".autofocus-status-line")).toHaveLength(1);
  });

  it("classifies a failure like the backend does", () => {
    for (const failure of ["Voltage change failed: x", "Failed to apply safe shutdown voltage: x", "Autofocus voltage step failed: x",
                           "No nanopositioner backend is available for the selected endpoint"]) {
      expect(isAutofocusFailureMessage(failure), failure).toBe(true);
    }
    for (const routine of ["Voltage: 12.5V", "Connected to oeabt nanopositioner on COM5", "Disconnected from nanopositioner", "Autofocus enabled"]) {
      expect(isAutofocusFailureMessage(routine), routine).toBe(false);
    }
  });
});
