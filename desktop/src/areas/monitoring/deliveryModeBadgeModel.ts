// The permanent acquisition-mode badge (#858, matrix SHELL-055): Qt's MainWindow::updateDeliveryModeBadge, from the backend's
// delivery mode (bridge ABI 45 fetch_frame_delivery_mode, #863). Pure: no React or Tauri imports.

/** The two fields the badge reads: `active_mode` is CaptureService::activeDeliveryMode() (the camera-confirmed mode while a
 *  capture runs, else the configured one) and `confirmed` is whether a running camera confirmed it. */
export interface DeliveryModeReading { valid: boolean; active_mode: string; confirmed: boolean }

export interface DeliveryModeBadge {
  /** Glyph and words fully identify the mode; colour is a supplementary cue (Qt). */
  text: string;
  tone: "every" | "latest";
}

/** Qt's tooltip, word for word. */
export const DELIVERY_MODE_TOOLTIP =
  "Every Frame preserves the complete acquisition sequence, at the cost of growing latency when the consumer falls behind.\n" +
  "Latest Frame minimizes latency by intentionally discarding stale frames, so recorded sequences may have gaps.";

/** Qt's badge text: "▶ EVERY FRAME · sequence preserved" or "⏩ LATEST FRAME · drops stale frames", with " (requested)" until
 *  a running camera confirms the mode. Null when there is no reading (no backend, an older bridge). */
export function deliveryModeBadge(reading: DeliveryModeReading | null | undefined): DeliveryModeBadge | null {
  if (!reading || !reading.valid) return null;
  const latest = reading.active_mode === "latestFrame";
  const text = latest ? "⏩ LATEST FRAME · drops stale frames" : "▶ EVERY FRAME · sequence preserved";
  return { text: reading.confirmed ? text : `${text} (requested)`, tone: latest ? "latest" : "every" };
}
