import type { HTMLAttributes } from "react";
import { cx } from "./classNames";

export interface StageTabsProps extends Omit<HTMLAttributes<HTMLElement>, "onSelect"> {
  /** Stage names in order, e.g. ["Prepare", "Align", "Experiment", "Review"]. */
  stages: string[];
  /** Index of the current stage (0-based). */
  active: number;
  /** Called with the stage index. Selecting a stage only navigates; it never actuates hardware. */
  onSelect?: (index: number) => void;
}

/** Numbered stage tabs ("01 Prepare", "02 Align", ...). The workflow order is the navigation. */
export function StageTabs({ stages, active, onSelect, className, ...rest }: StageTabsProps) {
  return (
    <nav {...rest} aria-label="Stages" className={cx(className)}>
      <ol className="mib-stages">
        {stages.map((stage, index) => (
          <li key={stage}>
            <button
              type="button"
              className="mib-stages__tab"
              aria-current={index === active ? "step" : undefined}
              onClick={() => onSelect?.(index)}
            >
              <span className="mib-stages__number">{String(index + 1).padStart(2, "0")}</span>
              {stage}
            </button>
          </li>
        ))}
      </ol>
    </nav>
  );
}
