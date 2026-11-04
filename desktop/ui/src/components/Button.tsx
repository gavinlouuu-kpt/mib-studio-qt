import type { ButtonHTMLAttributes } from "react";
import { cx } from "./classNames";

export type ButtonVariant = "primary" | "secondary" | "selected" | "destructive";

export interface ButtonProps extends ButtonHTMLAttributes<HTMLButtonElement> {
  /**
   * `primary` = the one main action of a view (dark), `secondary` = outlined,
   * `selected` = the active option of a segmented choice, `destructive` = stops
   * or discards. Use `disabled` for unavailable actions.
   */
  variant?: ButtonVariant;
  /** `small` for dense toolbars and table rows. */
  size?: "default" | "small";
}

/** Action button. One primary action per view; actions never fire on navigation. */
export function Button({ variant = "secondary", size = "default", className, type = "button", ...rest }: ButtonProps) {
  return (
    <button
      {...rest}
      type={type}
      className={cx("mib-button", `mib-button--${variant}`, size === "small" && "mib-button--small", className)}
    />
  );
}
