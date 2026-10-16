import type { HTMLAttributes, ReactNode } from "react";
import { cx } from "./classNames";

export interface StatTileProps extends HTMLAttributes<HTMLDivElement> {
  /** What is counted or measured, e.g. "Valid objects" or "median def.". */
  label: ReactNode;
  /** The number as reported by the backend, already formatted (e.g. "100 000"). */
  value: ReactNode;
  /** Optional delta against the reference run, e.g. "+0.027". Descriptive only. */
  delta?: ReactNode;
  /** Colours the value: `confirmed` for saved counts, `blocked` for losses. */
  tone?: "default" | "confirmed" | "warn" | "blocked";
}

/** A labelled number (accounting counts, Quick look medians). Numbers come from the backend. */
export function StatTile({ label, value, delta, tone = "default", className, ...rest }: StatTileProps) {
  return (
    <div {...rest} className={cx("mib-stat", className)}>
      <span className="mib-stat__label">{label}</span>
      <span className="mib-stat__row">
        <span className={cx("mib-stat__value", tone !== "default" && `mib-stat__value--${tone}`)}>{value}</span>
        {delta !== undefined ? <span className="mib-stat__delta">{delta}</span> : null}
      </span>
    </div>
  );
}
