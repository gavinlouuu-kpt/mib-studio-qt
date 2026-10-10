## 2026-10-10 — The SSD strip shows how long a stop has taken, and says why it takes that long (#667, UI only, no ABI change)

While the SSD reads STOPPING the strip shows "SSD writing the last records · N s" (the UI times the state; the first second shows no number) and the line "Stopping takes up to about 8 s at 5 kHz
(the frame ring is emptied onto the SSD)" below it (`stripView(status, stoppingSeconds)`, `STOP_NOTE` in `desktop/src/ssdView.ts`, `SsdStrip.tsx`). Why: a stop waits for the PL to drain
the frame ring onto the drive, up to ring ÷ drain rate: the S2 slot measured 7.8 s for the full ~297 MB ring at about 38 MB/s (overload at 5 kHz on this drive), Studio's own overhead was
about 0.12 s. The stop path is unchanged. Dismissing the "Save Experiment Data" prompt cancels the experiment by design (no run starts), which is not a Studio refusal.
