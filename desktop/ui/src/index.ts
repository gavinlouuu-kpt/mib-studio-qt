/**
 * @mib/ui — the shared UI language of MIB Studio, YOFO Studio and YOFO Review.
 *
 * Penpot reference: "UI language" page, board U0, plus the Operation app
 * (O0–O6) and Review app (R0–R6) pages. See ../penpot.json.
 *
 * Hosts import the stylesheets once: `import "@mib/ui/fonts.css"` (Inter faces)
 * and `import "@mib/ui/style.css"` (tokens and components). Wrap views in `AppShell`, or give the
 * container the `mib-root` class, so the font and base colours apply.
 */
import "./styles/tokens.css";
import "./styles/components.css";

export { UiRoot } from "./components/UiRoot";
export type { UiRootProps } from "./components/UiRoot";
export { StateChip } from "./components/StateChip";
export type { StateChipProps, StateTone } from "./components/StateChip";
export { Button } from "./components/Button";
export type { ButtonProps, ButtonVariant } from "./components/Button";
export { Card } from "./components/Card";
export type { CardProps } from "./components/Card";
export { FactList } from "./components/FactList";
export type { Fact, FactListProps } from "./components/FactList";
export { Toolbar } from "./components/Toolbar";
export type { ToolbarProps } from "./components/Toolbar";
export { StageTabs } from "./components/StageTabs";
export type { StageTabsProps } from "./components/StageTabs";
export { AppShell } from "./components/AppShell";
export type { AppShellProps, ShellContextItem } from "./components/AppShell";
export { StatTile } from "./components/StatTile";
export type { StatTileProps } from "./components/StatTile";
export { Stepper } from "./components/Stepper";
export type { StepperProps, StepperStep } from "./components/Stepper";
export { ConditionSwatch, conditionColor } from "./components/ConditionSwatch";
export type { ConditionSwatchProps, ConditionSlot } from "./components/ConditionSwatch";
export { RecordIdentity } from "./components/RecordIdentity";
export type { RecordIdentityProps } from "./components/RecordIdentity";
export { AreaDeformChart } from "./components/AreaDeformChart";
export type { AreaDeformChartProps, AreaDeformPoint, Isoelastic } from "./components/AreaDeformChart";
