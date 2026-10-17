import type { HTMLAttributes, ReactNode } from "react";
import { cx } from "./classNames";

export interface StepperStep {
  /** Step label, e.g. "Drain · finalizing". */
  label: ReactNode;
  /** Backend-confirmed progress of this step. */
  state: "done" | "current" | "pending" | "failed";
}

export interface StepperProps extends HTMLAttributes<HTMLOListElement> {
  /** Lifecycle steps in order (run lifecycle, export job, worker run). */
  steps: StepperStep[];
}

/** Vertical lifecycle stepper. A step is done only when the backend confirmed it. */
export function Stepper({ steps, className, ...rest }: StepperProps) {
  return (
    <ol {...rest} className={cx("mib-stepper", className)}>
      {steps.map((step, index) => (
        <li key={index} className={cx("mib-stepper__step", `mib-stepper__step--${step.state}`)} data-state={step.state}>
          <span className="mib-stepper__dot" aria-hidden="true">
            {step.state === "done" ? "✓" : step.state === "failed" ? "!" : ""}
          </span>
          {step.label}
        </li>
      ))}
    </ol>
  );
}
