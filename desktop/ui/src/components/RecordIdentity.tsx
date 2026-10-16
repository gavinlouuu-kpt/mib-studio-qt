import type { HTMLAttributes } from "react";
import { cx } from "./classNames";
import { FactList, type Fact } from "./FactList";

export interface RecordIdentityProps extends HTMLAttributes<HTMLDivElement> {
  /** Measurement record id as a string (u64 ids travel as strings end to end), e.g. "R-024210". */
  recordId: string;
  /** Frame index as a string. */
  frame: string;
  /** Object index within the frame. */
  object: string;
  /** Run index within the experiment. */
  run?: string;
  /** Sample reference from the sample sheet. */
  sample?: string;
  /** Provenance line: experiment id, method revision, E-LUT, calibration record. */
  provenance?: string;
  /** Evidence rows (image, mask, measurement) with their stored state. */
  evidence?: Fact[];
}

/** How one measured object is identified everywhere: record, frame, object, run, sample, provenance. */
export function RecordIdentity({ recordId, frame, object, run, sample, provenance, evidence, className, ...rest }: RecordIdentityProps) {
  const where = [`frame ${frame}`, `object ${object}`, run && `run ${run}`, sample && `sample ${sample}`].filter(Boolean).join(" · ");
  return (
    <div {...rest} className={cx("mib-record", className)}>
      <div className="mib-record__id">
        <span className="mib-record__name">{recordId}</span>
        <span className="mib-record__where">{where}</span>
        {provenance ? <span className="mib-record__provenance">{provenance}</span> : null}
      </div>
      {evidence?.length ? <FactList facts={evidence} /> : null}
    </div>
  );
}
