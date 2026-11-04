import { OptionGroup } from "@mib/ui";

export const SampleFormatCards = () => (
  <div style={{ width: 340 }}>
    <OptionGroup
      legend="Sample format"
      value="queue"
      onChange={() => {}}
      options={[
        { value: "single", label: "Single sample", description: "One run, one file" },
        { value: "queue", label: "Sample queue", description: "Manual tube swaps · one file per sample" },
        { value: "segments", label: "Time segments", unavailable: "Deferred · chapters (ADR 0015 A1)" },
        { value: "rack", label: "Tube rack / carousel", unavailable: "Unsupported · no autosampler" },
      ]}
    />
  </div>
);

export const ExportChoices = () => (
  <div style={{ width: 360 }}>
    <OptionGroup
      type="multiple"
      legend="What to export"
      value={["csv", "fcs"]}
      onChange={() => {}}
      options={[
        { value: "csv", label: "Metrics CSV per run", description: "All records or Population A · u64 ids as strings" },
        { value: "fcs", label: "FCS 3.x per run", description: "$MIB_RUN_ID · $SMNO = sample · $SRC = experiment" },
        { value: "pdf", label: "Report (PDF)", description: "Analysis plan, provenance, small multiples" },
      ]}
    />
  </div>
);

export const AutoStopList = () => (
  <div style={{ width: 260 }}>
    <OptionGroup
      variant="list"
      legend="Auto-stop"
      value="objects"
      onChange={() => {}}
      options={[
        { value: "manual", label: "Manual (default)" },
        { value: "objects", label: "Valid objects ≥ 100 000" },
        { value: "duration", label: "Duration 10 min" },
        { value: "both", label: "Both · whichever first" },
      ]}
    />
  </div>
);
