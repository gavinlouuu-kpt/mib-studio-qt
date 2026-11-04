import { useId, type FieldsetHTMLAttributes, type ReactNode } from "react";
import { cx } from "./classNames";
import { StateChip } from "./StateChip";

export interface OptionItem {
  /** Stable value reported by `onChange`. */
  value: string;
  /** Option name, e.g. "Sample queue". */
  label: ReactNode;
  /** One line under the label, e.g. "Manual tube swaps · one file per sample". */
  description?: ReactNode;
  /**
   * Why the option cannot be chosen, e.g. "Unsupported · no autosampler".
   * The option stays visible and inert with this reason as a grey chip.
   */
  unavailable?: ReactNode;
}

interface OptionGroupBase extends Omit<FieldsetHTMLAttributes<HTMLFieldSetElement>, "onChange" | "defaultValue"> {
  /** Group label, e.g. "Sample format". */
  legend: ReactNode;
  /** Hides the legend visually (keep it for screen readers). */
  hideLegend?: boolean;
  /** Options in display order. */
  options: OptionItem[];
  /** `cards` for choices with descriptions, `list` for short choices inside a card. */
  variant?: "cards" | "list";
}

export interface SingleOptionGroupProps extends OptionGroupBase {
  /** One choice (radio buttons). */
  type?: "single";
  /** The chosen value. */
  value: string | undefined;
  /** Reports the new draft value. */
  onChange: (value: string) => void;
}

export interface MultipleOptionGroupProps extends OptionGroupBase {
  /** Any number of choices (checkboxes). */
  type: "multiple";
  /** The chosen values. */
  value: string[];
  /** Reports the new draft values, in option order. */
  onChange: (value: string[]) => void;
}

export type OptionGroupProps = SingleOptionGroupProps | MultipleOptionGroupProps;

/**
 * Labelled choices shown together instead of a dropdown: sample format,
 * auto-stop, what to export. Choosing only changes the draft; the view's
 * primary action applies it. Unavailable options stay visible with the reason.
 */
export function OptionGroup(props: OptionGroupProps) {
  const { legend, hideLegend = false, options, variant = "cards", className, disabled, ...rest } = props;
  const name = useId();
  const multiple = props.type === "multiple";
  const chosen = new Set(multiple ? props.value : props.value === undefined ? [] : [props.value]);

  // Strip the union-only props before spreading onto the fieldset.
  const fieldsetProps = { ...rest } as Record<string, unknown>;
  delete fieldsetProps.type;
  delete fieldsetProps.value;
  delete fieldsetProps.onChange;

  function toggle(option: OptionItem, checked: boolean) {
    if (props.type === "multiple") {
      const next = options
        .filter((o) => (o.value === option.value ? checked : chosen.has(o.value)))
        .map((o) => o.value);
      props.onChange(next);
    } else if (checked) {
      props.onChange(option.value);
    }
  }

  return (
    <fieldset
      {...(fieldsetProps as FieldsetHTMLAttributes<HTMLFieldSetElement>)}
      disabled={disabled}
      className={cx("mib-options", `mib-options--${variant}`, multiple ? "mib-options--multiple" : "mib-options--single", className)}
    >
      <legend className={cx("mib-options__legend", hideLegend && "mib-visually-hidden")}>{legend}</legend>
      {options.map((option) => {
        const inert = option.unavailable !== undefined && option.unavailable !== null;
        const checked = chosen.has(option.value);
        return (
          <label
            key={option.value}
            className={cx(
              "mib-option",
              checked && "mib-option--checked",
              inert && "mib-option--unavailable",
            )}
          >
            <input
              className="mib-option__input"
              type={multiple ? "checkbox" : "radio"}
              name={name}
              value={option.value}
              checked={checked}
              disabled={inert || disabled}
              onChange={(event) => toggle(option, event.currentTarget.checked)}
            />
            <span className="mib-option__mark" aria-hidden="true" />
            <span className="mib-option__text">
              <span className="mib-option__label">{option.label}</span>
              {option.description ? <span className="mib-option__description">{option.description}</span> : null}
              {inert ? (
                <StateChip tone="unsupported" size="small" className="mib-option__reason">
                  {option.unavailable}
                </StateChip>
              ) : null}
            </span>
          </label>
        );
      })}
    </fieldset>
  );
}
