import { StateChip } from "@mib/ui";

const row = { display: "flex", flexWrap: "wrap" as const, gap: 8, alignItems: "center" };

export const AllTones = () => (
  <div style={row}>
    <StateChip tone="confirmed">Pass · Connected · Saved</StateChip>
    <StateChip tone="warn">Warn · 31 h · low n</StateChip>
    <StateChip tone="blocked">Fail · Blocked · loss</StateChip>
    <StateChip tone="unknown">Unknown · not reported</StateChip>
    <StateChip tone="unsupported">Unsupported · no sensor</StateChip>
    <StateChip tone="info">reference</StateChip>
    <StateChip tone="neutral">queued</StateChip>
  </div>
);

export const ReadinessGates = () => (
  <div style={row}>
    <StateChip tone="blocked">Focus: motor origin unknown</StateChip>
    <StateChip tone="warn">Calibration record 31 h old · warn</StateChip>
    <StateChip tone="confirmed">storage.budget · Pass</StateChip>
  </div>
);

export const SmallFlags = () => (
  <div style={row}>
    <StateChip tone="blocked" size="small">loss 0.7 %</StateChip>
    <StateChip tone="warn" size="small">stopped early</StateChip>
    <StateChip tone="neutral" size="small">E outside LUT</StateChip>
    <StateChip tone="confirmed" size="small">OK</StateChip>
  </div>
);
