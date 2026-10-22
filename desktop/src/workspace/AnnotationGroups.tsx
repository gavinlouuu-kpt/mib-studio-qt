import { useId, useState } from "react";
import { Button, Card, ConditionSwatch, DataTable, Notice, type DataTableColumn } from "@mib/ui";
import { annotationExport, annotationGroups, annotationLayers, type AnnotationGroup } from "./annotationModel";
import type { PlanDraft } from "./planModel";

export function AnnotationGroups({ draft }: { draft: PlanDraft }) {
  const id = useId();
  const [selected, setSelected] = useState("condition");
  const [showExport, setShowExport] = useState(false);
  const layers = annotationLayers(draft.rows);
  const layer = layers.find(l => l.id === selected) ?? layers[0];
  const groups = annotationGroups(draft.rows, layer);
  const output = annotationExport(draft, layer.id);
  const columns: DataTableColumn<AnnotationGroup>[] = [
    { key: "value", header: layer.label, cell: g => g.value === null ? "Not declared / invalid"
      : g.conditionSlot ? <ConditionSwatch condition={g.conditionSlot}>{g.value}</ConditionSwatch> : g.value },
    { key: "samples", header: "Sample refs", cell: g => g.sampleRefs.join(", ") },
    { key: "count", header: "Samples", numeric: true, cell: g => g.sampleRefs.length },
    { key: "units", header: "Declared unit labels", cell: g => g.declaredUnits.join(", ") || "Not declared" },
    { key: "missing", header: "Samples without unit", numeric: true, cell: g => g.missingUnitSamples },
  ];
  return <Card title="Annotation groups" subtitle="Planned sample annotations · no results or replicate inference" data-testid="annotation-groups"
    actions={<Button onClick={() => setShowExport(v => !v)} disabled={output.json === null}>
      {showExport ? "Hide annotation JSON" : "Show annotation JSON"}
    </Button>}>
    <label htmlFor={id}>Group samples by </label>
    <select id={id} value={layer.id} onChange={e => setSelected(e.target.value)}>
      {layers.map(l => <option key={l.id} value={l.id}>{l.label}</option>)}
    </select>
    <DataTable columns={columns} rows={groups} rowKey={g => g.key} density="compact"
      caption="Grouping does not change run order. Condition colours match the sample sheet." />
    <p className="ws-note">Add custom annotations using the sample editor's Key/values. Repeated unit labels stay the same unit;
      sample or cell counts do not establish independent replicates. Group selection is not saved across reloads.</p>
    {output.issues.length ? <Notice tone="warn" title="Correct the sample sheet before exporting annotations">
      <ul>{output.issues.map((issue, i) => <li key={i}>{issue}</li>)}</ul>
    </Notice> : null}
    {showExport && output.json !== null ? <div>
      <p className="ws-note">Copy this versioned JSON to export planned annotations and their grouping. This is not a saved experiment or acquisition evidence.</p>
      <textarea aria-label="Planned annotation JSON" readOnly rows={12} value={output.json} style={{ width: "100%" }} />
    </div> : null}
  </Card>;
}
