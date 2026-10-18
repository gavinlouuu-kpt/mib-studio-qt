import { Button, Card, FactList, StateChip } from "@mib/ui";

export const CalibrationRecord = () => (
  <div style={{ width: 440 }}>
    <Card
      title="Calibration record"
      subtitle="Rigid size beads · ADR 0015 item 7"
      status={
        <>
          <StateChip tone="warn">Applied · 31 h old</StateChip>
          <StateChip tone="neutral">policy: warn</StateChip>
        </>
      }
      actions={
        <>
          <Button variant="selected">Run calibration (≈ 2 min)</Button>
          <Button>Records</Button>
        </>
      }
    >
      <FactList
        facts={[
          { label: "Record", value: "cal-0042 · run R-0137 · 15.2 µm beads" },
          { label: "Pixel → µm", value: "0.4886 (applied)" },
          { label: "Deformation offset", value: "0.004 (check only)", tone: "confirmed" },
          { label: "Bound to", value: "chip C-2291 · 20× · ROI 512×96" },
        ]}
      />
    </Card>
  </div>
);

export const BlockedSystem = () => (
  <div style={{ width: 440 }}>
    <Card
      title="Focus system"
      subtitle="Motor coarse stage + piezo fine stage"
      status={<StateChip tone="blocked">Blocked · origin unknown</StateChip>}
      actions={<Button>Focus setup</Button>}
    >
      <FactList
        facts={[
          { label: "Coarse motor", value: "Found · origin unknown", tone: "blocked" },
          { label: "Piezo", value: "Connected · midrange", tone: "confirmed" },
        ]}
      />
    </Card>
  </div>
);

export const Tiles = () => (
  <div style={{ display: "flex", gap: 12, width: 560 }}>
    <Card variant="tile" title="Readiness" subtitle="Backend gates, Unknown/Unsupported, no actuation on navigation" />
    <Card variant="selected" title="03 cytoD 1" subtitle="just closed · 100 000 valid" />
  </div>
);
