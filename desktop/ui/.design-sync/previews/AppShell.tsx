import { AppShell, Button, Card, FactList, StateChip, Toolbar } from "@mib/ui";

export const OperationApp = () => (
  <AppShell
    brand="MIB Studio"
    tag="Imaging + cytometry"
    band={{ left: "Exp-0142 · CytoD dose, D1+D2", right: "Operation app" }}
    context={[
      { label: "Source", value: "Camera 01 / EGrabber" },
      { label: "Method", value: "Cell mechanics / v3" },
    ]}
    stages={["Prepare", "Align", "Experiment", "Review"]}
    activeStage={2}
    status={
      <>
        <StateChip tone="confirmed">Run closed · Exp-0142_03 · 4:21</StateChip>
        <StateChip tone="neutral">Queue paused · next: sample 4</StateChip>
      </>
    }
    headerActions={<Button>Settings</Button>}
  >
    <Toolbar title="Run summary">
      <Button>Plan</Button>
      <Button>Live</Button>
      <Button variant="selected">Run summary</Button>
      <Button variant="primary">Next sample</Button>
    </Toolbar>
    <div style={{ padding: "0 24px 24px", maxWidth: 520 }}>
      <Card title="Outcome" subtitle="Backend-confirmed · nothing here is invented by the UI" status={<StateChip tone="confirmed">Stop reason: target 100 000 valid objects reached</StateChip>}>
        <FactList
          facts={[
            { label: "Duration", value: "4 min 21 s" },
            { label: "File", value: "Exp-0142_03_D1_cytoD_1.h5 · 2.08 GB" },
            { label: "Writer", value: "closed · verified", tone: "confirmed" },
          ]}
        />
      </Card>
    </div>
  </AppShell>
);

export const ReviewApp = () => (
  <AppShell
    brand="YOFO Review"
    tag="Analysis · PC"
    context={[
      { label: "Source", value: "Exp-0142 · 7 runs" },
      { label: "Analysis plan", value: "CytoD-gates r4 · worker: Cytoflow" },
    ]}
    stages={["Open", "Overview", "Inspect", "Populations", "Compare", "Export"]}
    activeStage={1}
    status={<StateChip tone="neutral">Recorded files · read only</StateChip>}
    headerActions={<Button>Settings</Button>}
  >
    <Toolbar title="Overview">
      <Button variant="selected">Run overview</Button>
      <Button>Timeline</Button>
      <Button>Statistics</Button>
      <Button>Flags</Button>
    </Toolbar>
  </AppShell>
);
