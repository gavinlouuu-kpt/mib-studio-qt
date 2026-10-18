import type { HTMLAttributes, ReactNode } from "react";
import { cx } from "./classNames";

export interface NoticeProps extends Omit<HTMLAttributes<HTMLDivElement>, "title"> {
  /**
   * Backend state the notice reports: `warn` (attention, does not block),
   * `blocked` (blocks or data lost), `unknown` (not reported or no sensor),
   * `confirmed` (an outcome the backend confirmed), `info` (neutral).
   */
  tone: "warn" | "blocked" | "unknown" | "confirmed" | "info";
  /** One line in "State · detail" form, e.g. "Start blocked · 1 gate". */
  title: ReactNode;
  /** One line of detail with the facts behind the title. */
  children?: ReactNode;
  /** At most two Buttons, the main one last. */
  actions?: ReactNode;
}

/**
 * A banner across a view for a backend state that needs attention: overrides
 * that differ from the method, a blocked Start, an unknown reading, a closed
 * run. It reports; it never acts on its own.
 */
export function Notice({ tone, title, children, actions, className, ...rest }: NoticeProps) {
  return (
    <div {...rest} role="status" data-tone={tone} className={cx("mib-notice", `mib-notice--${tone}`, className)}>
      <div className="mib-notice__text">
        <div className="mib-notice__title">{title}</div>
        {children ? <div className="mib-notice__detail">{children}</div> : null}
      </div>
      {actions ? <div className="mib-notice__actions">{actions}</div> : null}
    </div>
  );
}
