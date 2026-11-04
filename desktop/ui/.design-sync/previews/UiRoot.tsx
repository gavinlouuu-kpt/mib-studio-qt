import { Button, Card, StateChip, UiRoot } from "@mib/ui";

export const WrappedPanel = () => (
  <UiRoot style={{ width: 420 }}>
    <Card
      title="Ready to operate?"
      subtitle="A single authoritative preflight decision"
      status={<StateChip tone="blocked">Start blocked · 1</StateChip>}
      actions={
        <>
          <Button variant="primary">View blockers</Button>
          <Button>Non-default settings (2)</Button>
        </>
      }
    />
  </UiRoot>
);
