import type { HTMLAttributes, ReactNode } from "react";
import { cx } from "./classNames";

export interface Fact {
  /** Fact name, e.g. "Coarse motor". */
  label: ReactNode;
  /** Value with its source, e.g. "requested 12.0 µm · read 12.0 µm". */
  value: ReactNode;
  /** Colours the value by backend state; omit for plain values. */
  tone?: "confirmed" | "warn" | "blocked" | "unknown";
}

export interface FactListProps extends HTMLAttributes<HTMLDListElement> {
  /** Label/value rows, in display order. */
  facts: Fact[];
}

/** Two-column label/value list used inside cards (readbacks, provenance, options). */
export function FactList({ facts, className, ...rest }: FactListProps) {
  return (
    <dl {...rest} className={cx("mib-facts", className)}>
      {facts.map((fact, index) => (
        <div key={index} style={{ display: "contents" }}>
          <dt className="mib-facts__label">{fact.label}</dt>
          <dd className={cx("mib-facts__value", fact.tone && `mib-facts__value--${fact.tone}`)}>{fact.value}</dd>
        </div>
      ))}
    </dl>
  );
}
