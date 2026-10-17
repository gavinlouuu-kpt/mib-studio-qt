import { RecordIdentity } from "@mib/ui";

export const InspectedRecord = () => (
  <div style={{ width: 360 }}>
    <RecordIdentity
      recordId="R-024210"
      frame="82 140"
      object="02"
      run="3"
      sample="03"
      provenance="experiment 7c1e… · method v3 rev 8f2a · lut-v2 · cal-0042"
      evidence={[
        { label: "Image", value: "Saved", tone: "confirmed" },
        { label: "Mask / contour", value: "Saved", tone: "confirmed" },
        { label: "Measurement", value: "Stored (at acquisition)", tone: "confirmed" },
      ]}
    />
  </div>
);

export const MissingEvidence = () => (
  <div style={{ width: 360 }}>
    <RecordIdentity
      recordId="R-024231"
      frame="82 147"
      object="01"
      evidence={[
        { label: "Image", value: "Saved", tone: "confirmed" },
        { label: "Mask / contour", value: "Missing (not recorded)", tone: "unknown" },
      ]}
    />
  </div>
);
