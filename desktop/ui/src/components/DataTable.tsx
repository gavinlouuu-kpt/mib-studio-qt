import type { KeyboardEvent, MouseEvent, ReactNode, TableHTMLAttributes } from "react";
import { cx } from "./classNames";

export interface DataTableColumn<Row> {
  /** Stable column id; also the row field shown when `cell` is omitted. */
  key: string;
  /** Header label, e.g. "Median E (kPa)". */
  header: ReactNode;
  /**
   * Cell content. Return `null` or `undefined` when the value was not
   * recorded: the table shows it as missing, never as a guess.
   * Defaults to `row[key]`.
   */
  cell?: (row: Row) => ReactNode;
  /** Right-aligns the column and uses tabular figures (counts, measurements). */
  numeric?: boolean;
  /** Bold values, for the identifying column (run, record). */
  emphasis?: boolean;
  /** CSS width, e.g. "96px" or "20%". */
  width?: string;
}

export interface DataTableProps<Row>
  extends Omit<TableHTMLAttributes<HTMLTableElement>, "children" | "onSelect"> {
  /** Columns in display order. */
  columns: DataTableColumn<Row>[];
  /** Rows in display order. */
  rows: Row[];
  /** Row id as a string (record, run or sample id; u64 ids travel as strings). */
  rowKey: (row: Row) => string;
  /** The selected row's id: the inspected record (marked `aria-current`). */
  selectedKey?: string;
  /** Makes rows selectable (click, Enter or Space). */
  onSelect?: (key: string, row: Row) => void;
  /** What to say when there are no rows, e.g. "No runs recorded yet." */
  empty?: ReactNode;
  /** Shown for a value that was not recorded. Defaults to an en dash. */
  missing?: ReactNode;
  /** Optional caption above the table. */
  caption?: ReactNode;
  /** `compact` for sheets and dense lists inside cards. */
  density?: "default" | "compact";
}

// A click or key press on a control inside a cell (button, link, input) belongs to that control.
const INTERACTIVE = "a, button, input, select, textarea, label, [contenteditable], [role=button]";

function fromCellControl(event: { target: EventTarget; currentTarget: EventTarget }): boolean {
  const target = event.target as Element | null;
  const control = target?.closest?.(INTERACTIVE);
  return Boolean(control && control !== event.currentTarget && (event.currentTarget as Element).contains(control));
}

function isMissing(value: ReactNode): boolean {
  return value === null || value === undefined || value === "";
}

/**
 * Rows of recorded facts: per-run statistics, linked records, sample sheets,
 * experiment libraries and export previews. The table never computes or fills
 * in a value; a missing value is shown as missing.
 */
export function DataTable<Row>({
  columns,
  rows,
  rowKey,
  selectedKey,
  onSelect,
  empty = "Nothing to show.",
  missing = "–",
  caption,
  density = "default",
  className,
  ...rest
}: DataTableProps<Row>) {
  const selectable = Boolean(onSelect);

  function select(row: Row) {
    onSelect?.(rowKey(row), row);
  }

  function onClick(event: MouseEvent<HTMLTableRowElement>, row: Row) {
    if (fromCellControl(event)) return;
    select(row);
  }

  function onKeyDown(event: KeyboardEvent<HTMLTableRowElement>, row: Row) {
    // Only the focused row itself: Enter or Space in a cell control keeps its own meaning.
    if (event.target !== event.currentTarget) return;
    if (event.key === "Enter" || event.key === " ") {
      event.preventDefault();
      select(row);
    }
  }

  return (
    <table
      {...rest}
      className={cx("mib-table", density === "compact" && "mib-table--compact", selectable && "mib-table--selectable", className)}
    >
      {caption ? <caption className="mib-table__caption">{caption}</caption> : null}
      <thead>
        <tr>
          {columns.map((column) => (
            <th
              key={column.key}
              scope="col"
              style={column.width ? { width: column.width } : undefined}
              className={cx("mib-table__head", column.numeric && "mib-table__cell--numeric")}
            >
              {column.header}
            </th>
          ))}
        </tr>
      </thead>
      <tbody>
        {rows.length === 0 ? (
          <tr className="mib-table__empty-row">
            <td className="mib-table__empty" colSpan={columns.length}>
              {empty}
            </td>
          </tr>
        ) : (
          rows.map((row) => {
            const key = rowKey(row);
            const selected = selectedKey !== undefined && key === selectedKey;
            return (
              <tr
                key={key}
                className={cx("mib-table__row", selected && "mib-table__row--selected")}
                aria-current={selected ? "true" : undefined}
                tabIndex={selectable ? 0 : undefined}
                onClick={selectable ? (event) => onClick(event, row) : undefined}
                onKeyDown={selectable ? (event) => onKeyDown(event, row) : undefined}
              >
                {columns.map((column) => {
                  const value = column.cell
                    ? column.cell(row)
                    : ((row as Record<string, unknown>)[column.key] as ReactNode);
                  const absent = isMissing(value);
                  return (
                    <td
                      key={column.key}
                      className={cx(
                        "mib-table__cell",
                        column.numeric && "mib-table__cell--numeric",
                        column.emphasis && "mib-table__cell--emphasis",
                        absent && "mib-table__cell--missing",
                      )}
                      title={absent ? "Not recorded" : undefined}
                    >
                      {absent ? missing : value}
                    </td>
                  );
                })}
              </tr>
            );
          })
        )}
      </tbody>
    </table>
  );
}
