// The permanent acquisition-mode badge in the status bar (#858, SHELL-055; Qt's status-bar permanent widget, visible on every
// stage). It reads the backend's delivery mode itself, every 2 s and at once when the camera starts or stops (the start confirms
// the mode), so a mode set by a profile, a central method or the Connect stage shows wherever the operator is.
import { useEffect, useState } from "react";
import { bridge } from "../../bridge";
import { useStudio } from "../../studio/StudioContext";
import { DELIVERY_MODE_TOOLTIP, deliveryModeBadge, type DeliveryModeReading } from "./deliveryModeBadgeModel";

export const DELIVERY_MODE_POLL_MS = 2000;

export function DeliveryModeBadge() {
  const { ready, running, instrumentModes } = useStudio();
  const [reading, setReading] = useState<DeliveryModeReading | null>(null);
  useEffect(() => {
    // The PZ7035 has no live-camera delivery mode in Run (its readiness has no camera.deliveryMode gate either).
    if (!ready || instrumentModes) { setReading(null); return; }
    let alive = true, polling = false;
    const poll = async () => {
      if (polling) return;
      polling = true;
      try {
        const status = await bridge.fetchFrameDeliveryMode();
        if (alive) setReading(status.valid ? status : null);
      } catch {
        if (alive) setReading(null); // an older bridge or a failed read: no badge rather than a stale one
      } finally { polling = false; }
    };
    void poll();
    const id = window.setInterval(() => void poll(), DELIVERY_MODE_POLL_MS);
    return () => { alive = false; window.clearInterval(id); };
  }, [ready, instrumentModes, running]);
  const badge = deliveryModeBadge(reading);
  if (!badge) return null;
  return (
    <span className={`delivery-mode-badge ${badge.tone}`} title={DELIVERY_MODE_TOOLTIP} data-tour="monitoring-delivery-mode-badge">
      {badge.text}
    </span>
  );
}
