import type { HTMLAttributes, ReactNode } from "react";
import { cx } from "./classNames";

export interface CardProps extends Omit<HTMLAttributes<HTMLElement>, "title"> {
  /** Card title, e.g. "Calibration record". */
  title: ReactNode;
  /** One-line subtitle: what this is, where it comes from, how fresh. */
  subtitle?: ReactNode;
  /** State chips shown under the title (usually one or two `StateChip`s). */
  status?: ReactNode;
  /** Buttons at the bottom of the card. */
  actions?: ReactNode;
  /** `default` = bordered card, `tile` = grey inner tile, `selected` = highlighted choice. */
  variant?: "default" | "tile" | "selected";
}

/**
 * The card anatomy every view uses: title, one-line subtitle, state chips,
 * facts, then actions. Facts never guess — a missing source shows Unknown.
 */
export function Card({ title, subtitle, status, actions, variant = "default", className, children, ...rest }: CardProps) {
  return (
    <section {...rest} className={cx("mib-card", variant !== "default" && `mib-card--${variant}`, className)}>
      <header className="mib-card__head">
        <h3 className="mib-card__title">{title}</h3>
        {subtitle ? <p className="mib-card__subtitle">{subtitle}</p> : null}
      </header>
      {status ? <div className="mib-card__status">{status}</div> : null}
      {children ? <div className="mib-card__body">{children}</div> : null}
      {actions ? <div className="mib-card__actions">{actions}</div> : null}
    </section>
  );
}
