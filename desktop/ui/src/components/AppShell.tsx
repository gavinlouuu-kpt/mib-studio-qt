import type { HTMLAttributes, ReactNode } from "react";
import { cx } from "./classNames";
import { StageTabs } from "./StageTabs";

export interface ShellContextItem {
  /** Small caps label, e.g. "Source" or "Method". */
  label: string;
  /** Value, e.g. "Camera 01 / EGrabber". */
  value: ReactNode;
}

export interface AppShellProps extends HTMLAttributes<HTMLDivElement> {
  /** Product name: "MIB Studio", "YOFO Studio" or "YOFO Review". Components do not change with the brand. */
  brand: string;
  /** Short tag next to the brand, e.g. "Imaging + cytometry". */
  tag?: string;
  /** Up to two context items in the header (source, method, analysis plan). */
  context?: ShellContextItem[];
  /** Stage names. Operation app: Prepare, Align, Experiment, Review. Review app: Open … Export. */
  stages: string[];
  /** Index of the current stage (0-based). */
  activeStage: number;
  /** Called with a stage index when a stage tab is clicked. */
  onStageSelect?: (index: number) => void;
  /** Status pills at the right of the stage row (`StateChip`s, at most two). */
  status?: ReactNode;
  /** Header actions at the far right, e.g. a Settings `Button`. */
  headerActions?: ReactNode;
  /** Optional thin band above the header (left and right text). */
  band?: { left?: ReactNode; right?: ReactNode };
  /** The view: usually a `Toolbar` followed by a grid of `Card`s. */
  children?: ReactNode;
}

/**
 * The shared shell of both apps: band, header (brand, context, actions), the
 * numbered stage tabs with status pills, then the view.
 */
export function AppShell({
  brand,
  tag,
  context = [],
  stages,
  activeStage,
  onStageSelect,
  status,
  headerActions,
  band,
  className,
  children,
  ...rest
}: AppShellProps) {
  return (
    <div {...rest} className={cx("mib-root", "mib-shell", className)}>
      {band ? (
        <div className="mib-shell__band">
          <strong>{band.left}</strong>
          <span>{band.right}</span>
        </div>
      ) : null}
      <header className="mib-shell__header">
        <div className="mib-shell__brand">
          <h1 className="mib-shell__name">{brand}</h1>
          {tag ? <span className="mib-shell__tag">{tag}</span> : null}
        </div>
        {context.length ? (
          <dl className="mib-shell__context">
            {context.map((item) => (
              <div key={item.label} className="mib-shell__context-item">
                <dt className="mib-shell__context-label">{item.label}</dt>
                <dd className="mib-shell__context-value">{item.value}</dd>
              </div>
            ))}
          </dl>
        ) : null}
        {headerActions}
      </header>
      <div className="mib-shell__nav">
        <StageTabs stages={stages} active={activeStage} onSelect={onStageSelect} />
        {status ? <div className="mib-shell__pills">{status}</div> : null}
      </div>
      <main className="mib-shell__main">{children}</main>
    </div>
  );
}
