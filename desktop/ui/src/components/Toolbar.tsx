import type { HTMLAttributes, ReactNode } from "react";
import { cx } from "./classNames";

export interface ToolbarProps extends Omit<HTMLAttributes<HTMLDivElement>, "title"> {
  /** View title shown at the right, e.g. "Run summary". */
  title?: ReactNode;
  /** One row of `Button`s: sub-views first, then the view's actions. */
  children: ReactNode;
}

/** One row of actions under the stage tabs, with the view title at the right. */
export function Toolbar({ title, className, children, ...rest }: ToolbarProps) {
  return (
    <div {...rest} role="toolbar" className={cx("mib-toolbar", className)}>
      {children}
      {title ? <span className="mib-toolbar__title">{title}</span> : null}
    </div>
  );
}
