import type { HTMLAttributes, ReactNode } from "react";
import { cx } from "./classNames";

/** Condition slot 1–5, in sample-sheet order. Slot 5 is calibration / reference. */
export type ConditionSlot = 1 | 2 | 3 | 4 | 5;

export interface ConditionSwatchProps extends HTMLAttributes<HTMLSpanElement> {
  /** Which sample-sheet condition this is. The same slot keeps the same colour in every view. */
  condition: ConditionSlot;
  /** Condition label, e.g. "CytoD 1 µM". */
  children: ReactNode;
  /** `large` for legends and colour keys. */
  size?: "default" | "large";
}

/** CSS colour of a condition slot, for charts and custom marks. */
export function conditionColor(condition: ConditionSlot): string {
  return `var(--mib-condition-${condition})`;
}

/** Colour key for an experimental condition. Never use a condition colour for a state. */
export function ConditionSwatch({ condition, size = "default", className, children, ...rest }: ConditionSwatchProps) {
  return (
    <span {...rest} className={cx("mib-condition", size === "large" && "mib-condition--large", className)}>
      <span className="mib-condition__swatch" style={{ background: conditionColor(condition) }} aria-hidden="true" />
      {children}
    </span>
  );
}
