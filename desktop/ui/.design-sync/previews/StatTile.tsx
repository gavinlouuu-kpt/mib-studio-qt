import { StatTile } from "@mib/ui";

export const RunCounts = () => (
  <div style={{ display: "flex", flexDirection: "column", gap: 10 }}>
    <StatTile label="Frames received" value="41 982" />
    <StatTile label="Valid objects" value="100 000" />
    <StatTile label="Saved cell records" value="100 000" tone="confirmed" />
    <StatTile label="Dropped" value="0" tone="confirmed" />
  </div>
);

export const QuickLookDeltas = () => (
  <div style={{ display: "flex", gap: 32 }}>
    <StatTile label="median def." value="0.072" delta="+0.027" />
    <StatTile label="median area" value="60.9" delta="−1.2" />
    <StatTile label="median E" value="0.98" delta="−0.44" />
  </div>
);

export const Loss = () => <StatTile label="Dropped at 0:41–0:44" value="312" tone="blocked" />;
