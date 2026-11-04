import { ConditionSwatch, DataTable, StateChip } from "@mib/ui";

type Run = { run: string; slot: 1 | 2 | 3 | 4 | 5; condition: string; n: string; def: string; e?: string; flags: Array<[string, "blocked" | "warn" | "unknown"]> };

const runs: Run[] = [
  { run: "01", slot: 1, condition: "Vehicle", n: "100 000", def: "0.045", e: "1.42", flags: [] },
  { run: "03", slot: 3, condition: "CytoD 1 µM", n: "100 000", def: "0.072", e: "0.98", flags: [] },
  { run: "05", slot: 2, condition: "CytoD 0.1 µM", n: "43 377", def: "0.057", e: "1.22", flags: [["loss 0.7 %", "blocked"], ["stopped early", "warn"]] },
  { run: "07", slot: 5, condition: "Calibration beads", n: "18 870", def: "0.004", flags: [["E outside LUT", "unknown"]] },
];

export const PerRunStatistics = () => (
  <div style={{ width: 720 }}>
    <DataTable
      caption="Per-run statistics"
      rowKey={(r) => r.run}
      rows={runs}
      columns={[
        { key: "run", header: "Run", emphasis: true },
        { key: "condition", header: "Condition", cell: (r) => <ConditionSwatch condition={r.slot}>{r.condition}</ConditionSwatch> },
        { key: "n", header: "n valid", numeric: true },
        { key: "def", header: "Median def.", numeric: true },
        { key: "e", header: "Median E (kPa)", numeric: true },
        {
          key: "flags",
          header: "Flags",
          // An empty list renders nothing: no flags is a fact, not a missing value.
          cell: (r) => r.flags.map(([label, tone]) => <StateChip key={label} tone={tone} size="small">{label}</StateChip>),
        },
      ]}
    />
  </div>
);

type Rec = { id: string; frame: string; object: string; area: string; evidence: string };
const records: Rec[] = [
  { id: "R-024210", frame: "82 140", object: "02", area: "186.8", evidence: "image · mask" },
  { id: "R-024231", frame: "82 147", object: "01", area: "241.6", evidence: "image · mask missing" },
  { id: "R-024244", frame: "82 151", object: "03", area: "174.0", evidence: "image · mask" },
];

export const LinkedRecordsSelected = () => (
  <div style={{ width: 560 }}>
    <DataTable
      caption="Linked cell records"
      rowKey={(r) => r.id}
      rows={records}
      selectedKey="R-024210"
      onSelect={() => {}}
      columns={[
        { key: "id", header: "Record", emphasis: true },
        { key: "frame", header: "Frame", numeric: true },
        { key: "object", header: "Object", numeric: true },
        { key: "area", header: "Area (µm²)", numeric: true },
        { key: "evidence", header: "Evidence" },
      ]}
    />
  </div>
);

type Sample = { i: string; file: string; slot: 1 | 2 | 5; condition: string; conc?: string };
const samples: Sample[] = [
  { i: "1", file: "Exp-0142_01_D1_vehicle.h5", slot: 1, condition: "Vehicle", conc: "0" },
  { i: "2", file: "Exp-0142_02_D1_cytoD_0p1.h5", slot: 2, condition: "CytoD", conc: "0.1 µM" },
  { i: "3", file: "Exp-0142_07_beads_15um.h5", slot: 5, condition: "Beads" },
];

export const CompactSampleSheet = () => (
  <div style={{ width: 620 }}>
    <DataTable
      density="compact"
      caption="Sample sheet"
      rowKey={(r) => r.i}
      rows={samples}
      columns={[
        { key: "i", header: "#", width: "32px" },
        { key: "file", header: "Sample / run file" },
        { key: "condition", header: "Condition", cell: (r) => <ConditionSwatch condition={r.slot}>{r.condition}</ConditionSwatch> },
        { key: "conc", header: "Conc.", numeric: true },
      ]}
    />
  </div>
);

export const EmptyExperiment = () => (
  <div style={{ width: 520 }}>
    <DataTable
      rowKey={(r: Run) => r.run}
      rows={[]}
      empty="No runs recorded in this experiment yet."
      columns={[
        { key: "run", header: "Run" },
        { key: "condition", header: "Condition" },
        { key: "n", header: "n valid", numeric: true },
      ]}
    />
  </div>
);
