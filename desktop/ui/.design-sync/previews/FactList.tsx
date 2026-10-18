import { FactList } from "@mib/ui";

export const FocusReadbacks = () => (
  <FactList
    facts={[
      { label: "Coarse motor", value: "Found · origin unknown", tone: "blocked" },
      { label: "Piezo", value: "Connected · midrange", tone: "confirmed" },
      { label: "Protection", value: "Travel limits recognized", tone: "confirmed" },
      { label: "Readback", value: "requested 12.0 µm · read 12.0 µm" },
    ]}
  />
);

export const Fluidics = () => (
  <FactList
    facts={[
      { label: "Sample pump", value: "Tushui · 0.040 µL/s configured" },
      { label: "Sheath pump", value: "Tushui · 0.120 µL/s configured" },
      { label: "Delivered", value: "≈ 412 µL (from speed, 25 µL/rev)", tone: "warn" },
      { label: "Reservoir level", value: "Unknown · no sensor fitted", tone: "unknown" },
    ]}
  />
);
