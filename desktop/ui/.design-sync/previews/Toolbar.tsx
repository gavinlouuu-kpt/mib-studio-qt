import { Button, Toolbar } from "@mib/ui";

export const RunSummary = () => (
  <Toolbar title="Run summary">
    <Button>Plan</Button>
    <Button>Live</Button>
    <Button variant="selected">Run summary</Button>
    <Button variant="primary">Next sample</Button>
    <Button>End queue</Button>
  </Toolbar>
);

export const ReviewExport = () => (
  <Toolbar title="Export">
    <Button variant="selected">Metrics</Button>
    <Button>Images</Button>
    <Button>FCS</Button>
    <Button>Statistics</Button>
    <Button>Report</Button>
  </Toolbar>
);
