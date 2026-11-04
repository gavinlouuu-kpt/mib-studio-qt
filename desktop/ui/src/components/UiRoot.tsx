import type { HTMLAttributes } from "react";
import { cx } from "./classNames";

export type UiRootProps = HTMLAttributes<HTMLDivElement>;

/**
 * Root wrapper that applies the UI language font, base text colour and box
 * sizing. `AppShell` includes it; wrap anything rendered outside an
 * `AppShell` (dialogs, embedded panels) in `UiRoot`.
 */
export function UiRoot({ className, ...rest }: UiRootProps) {
  return <div {...rest} className={cx("mib-root", className)} />;
}
