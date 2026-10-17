import { ConditionSwatch } from "@mib/ui";

const col = { display: "flex", flexDirection: "column" as const, gap: 12 };

export const ColourKey = () => (
  <div style={col}>
    <ConditionSwatch condition={1} size="large">Condition A (e.g. vehicle)</ConditionSwatch>
    <ConditionSwatch condition={2} size="large">Condition B · low dose</ConditionSwatch>
    <ConditionSwatch condition={3} size="large">Condition B · high dose</ConditionSwatch>
    <ConditionSwatch condition={4} size="large">Condition C</ConditionSwatch>
    <ConditionSwatch condition={5} size="large">Calibration / reference</ConditionSwatch>
  </div>
);

export const InlineLegend = () => (
  <div style={{ display: "flex", gap: 20 }}>
    <ConditionSwatch condition={1}>01 vehicle</ConditionSwatch>
    <ConditionSwatch condition={2}>02 CytoD 0.1 µM</ConditionSwatch>
    <ConditionSwatch condition={3}>03 CytoD 1 µM</ConditionSwatch>
    <ConditionSwatch condition={5}>07 beads</ConditionSwatch>
  </div>
);
