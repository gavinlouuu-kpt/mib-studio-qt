import { describe, expect, it } from "vitest";
import { DELIVERY_MODE_TOOLTIP, deliveryModeBadge } from "./deliveryModeBadgeModel";

// #858 (SHELL-055): Qt's MainWindow::updateDeliveryModeBadge.
describe("the acquisition-mode badge", () => {
  it("says the confirmed mode as Qt does", () => {
    expect(deliveryModeBadge({ valid: true, active_mode: "everyFrame", confirmed: true })).toEqual({ text: "▶ EVERY FRAME · sequence preserved", tone: "every" });
    expect(deliveryModeBadge({ valid: true, active_mode: "latestFrame", confirmed: true })).toEqual({ text: "⏩ LATEST FRAME · drops stale frames", tone: "latest" });
  });
  it("adds (requested) until a running camera confirms the mode", () => {
    expect(deliveryModeBadge({ valid: true, active_mode: "latestFrame", confirmed: false })?.text).toBe("⏩ LATEST FRAME · drops stale frames (requested)");
    expect(deliveryModeBadge({ valid: true, active_mode: "everyFrame", confirmed: false })?.text).toBe("▶ EVERY FRAME · sequence preserved (requested)");
  });
  it("shows nothing without a reading", () => {
    expect(deliveryModeBadge(null)).toBeNull();
    expect(deliveryModeBadge({ valid: false, active_mode: "", confirmed: false })).toBeNull();
  });
  it("keeps Qt's tooltip", () => {
    expect(DELIVERY_MODE_TOOLTIP).toBe("Every Frame preserves the complete acquisition sequence, at the cost of growing latency when the consumer falls behind.\n" +
      "Latest Frame minimizes latency by intentionally discarding stale frames, so recorded sequences may have gaps.");
  });
});
