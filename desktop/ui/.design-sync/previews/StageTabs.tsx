import { StageTabs } from "@mib/ui";

export const OperationApp = () => <StageTabs stages={["Prepare", "Align", "Experiment", "Review"]} active={1} />;

export const ReviewApp = () => (
  <StageTabs stages={["Open", "Overview", "Inspect", "Populations", "Compare", "Export"]} active={2} />
);
