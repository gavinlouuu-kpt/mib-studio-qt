import { Button } from "@mib/ui";

const row = { display: "flex", flexWrap: "wrap" as const, gap: 10, alignItems: "center" };

export const Variants = () => (
  <div style={row}>
    <Button variant="primary">Primary action</Button>
    <Button>Secondary action</Button>
    <Button variant="selected">Selected</Button>
    <Button variant="destructive">Destructive</Button>
    <Button disabled>Unavailable</Button>
  </div>
);

export const RunDecisions = () => (
  <div style={row}>
    <Button variant="primary">Next sample</Button>
    <Button>Repeat this sample</Button>
    <Button>End queue</Button>
  </div>
);

export const Small = () => (
  <div style={row}>
    <Button size="small">Previous record</Button>
    <Button size="small">Next record</Button>
    <Button size="small" variant="selected">Contour</Button>
  </div>
);
