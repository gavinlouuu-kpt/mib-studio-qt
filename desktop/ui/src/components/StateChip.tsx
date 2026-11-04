import type { HTMLAttributes, ReactNode } from "react";
import { cx } from "./classNames";

/**
 * Backend state as the UI language shows it. `confirmed` = reported and good,
 * `warn` = needs attention but does not block, `blocked` = blocks or data lost,
 * `unknown` = not read yet (never shown as pass), `unsupported` = capability
 * absent (never blocks), `info` = informational (inspected / reference),
 * `neutral` = plain label such as "queued".
 */
export type StateTone =
  | "confirmed"
  | "warn"
  | "blocked"
  | "unknown"
  | "unsupported"
  | "info"
  | "neutral";

export interface StateChipProps extends Omit<HTMLAttributes<HTMLSpanElement>, "children"> {
  /** Which backend state the chip reports. */
  tone: StateTone;
  /** Short label, e.g. "Pass · 0.91" or "Unknown · not reported". */
  children: ReactNode;
  /** Compact chip for dense tables and flag columns. */
  size?: "default" | "small";
}

/**
 * A state chip: the only way the UI shows a backend state. The tone must come
 * from the backend report; the UI never upgrades Unknown to Pass.
 */
export function StateChip({ tone, size = "default", className, children, ...rest }: StateChipProps) {
  return (
    <span
      {...rest}
      data-tone={tone}
      className={cx("mib-chip", `mib-chip--${tone}`, size === "small" && "mib-chip--small", className)}
    >
      {children}
    </span>
  );
}
