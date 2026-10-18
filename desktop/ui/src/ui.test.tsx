// @vitest-environment jsdom
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import {
  AppShell,
  AreaDeformChart,
  Button,
  Card,
  CellImage,
  CellStrip,
  ConditionSwatch,
  DataTable,
  FactList,
  Notice,
  OptionGroup,
  RecordIdentity,
  StateChip,
  StatTile,
  Stepper,
  Toolbar,
  conditionColor,
} from "./index";

let host: HTMLDivElement, root: Root;
beforeEach(() => {
  (globalThis as { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true;
  host = document.createElement("div");
  document.body.appendChild(host);
  root = createRoot(host);
});
afterEach(() => {
  act(() => root.unmount());
  host.remove();
});
const render = (node: React.ReactNode) => act(() => root.render(node));

describe("UI language kit", () => {
  it("shows the backend tone on a state chip and never changes it", () => {
    render(<StateChip tone="unknown">Unknown · not reported</StateChip>);
    const chip = host.querySelector(".mib-chip")!;
    expect(chip.className).toContain("mib-chip--unknown");
    expect(chip.getAttribute("data-tone")).toBe("unknown");
  });

  it("renders buttons as type=button with the chosen variant", () => {
    render(<Button variant="primary">Start</Button>);
    const button = host.querySelector("button")!;
    expect(button.type).toBe("button");
    expect(button.className).toContain("mib-button--primary");
  });

  it("lays out a card as title, subtitle, status, facts, actions", () => {
    render(
      <Card title="Focus system" subtitle="Motor coarse stage" status={<StateChip tone="blocked">Blocked</StateChip>} actions={<Button>Focus setup</Button>}>
        <FactList facts={[{ label: "Coarse motor", value: "origin unknown", tone: "blocked" }]} />
      </Card>,
    );
    const parts = [...host.querySelector(".mib-card")!.children].map((el) => el.className);
    expect(parts).toEqual(["mib-card__head", "mib-card__status", "mib-card__body", "mib-card__actions"]);
    expect(host.querySelector(".mib-facts__value--blocked")!.textContent).toBe("origin unknown");
  });

  it("marks the active stage and reports clicks without acting on its own", () => {
    const onStageSelect = vi.fn();
    render(
      <AppShell brand="MIB Studio" stages={["Prepare", "Align", "Experiment", "Review"]} activeStage={2} onStageSelect={onStageSelect}>
        <Toolbar title="Run summary">
          <Button>Plan</Button>
        </Toolbar>
      </AppShell>,
    );
    const tabs = [...host.querySelectorAll<HTMLButtonElement>(".mib-stages__tab")];
    expect(tabs.map((t) => t.textContent)).toEqual(["01Prepare", "02Align", "03Experiment", "04Review"]);
    expect(tabs[2].getAttribute("aria-current")).toBe("step");
    act(() => tabs[0].click());
    expect(onStageSelect).toHaveBeenCalledWith(0);
  });

  it("renders stepper states and stat deltas", () => {
    render(
      <>
        <Stepper steps={[{ label: "Queued", state: "done" }, { label: "Writing", state: "current" }, { label: "Done", state: "pending" }]} />
        <StatTile label="median def." value="0.072" delta="+0.027" />
      </>,
    );
    expect([...host.querySelectorAll("[data-state]")].map((s) => s.getAttribute("data-state"))).toEqual(["done", "current", "pending"]);
    expect(host.querySelector(".mib-stat__delta")!.textContent).toBe("+0.027");
  });

  it("identifies a record with string ids", () => {
    render(<RecordIdentity recordId="R-024210" frame="82140" object="02" run="3" sample="03" />);
    expect(host.querySelector(".mib-record__where")!.textContent).toBe("frame 82140 · object 02 · run 3 · sample 03");
  });

  it("colours conditions by slot and plots polygon, inspected record and E curves", () => {
    expect(conditionColor(3)).toBe("var(--mib-condition-3)");
    render(
      <>
        <ConditionSwatch condition={1}>Vehicle</ConditionSwatch>
        <AreaDeformChart
          points={[{ area: 150, deformability: 0.1, condition: 1 }, { area: 100, deformability: 0.05 }]}
          isoelastics={[{ label: "1.0", points: [{ area: 0, deformability: 0 }, { area: 300, deformability: 0.15 }] }]}
          polygon={[{ area: 90, deformability: 0.04 }, { area: 200, deformability: 0.14 }, { area: 230, deformability: 0.04 }]}
          inspected={{ area: 186.8, deformability: 0.0612, label: "R-024210" }}
        />
      </>,
    );
    expect(host.querySelectorAll("circle[r='2']")).toHaveLength(2);
    expect(host.querySelectorAll(".mib-chart__handle")).toHaveLength(3);
    expect(host.querySelector(".mib-chart__inspected-label")!.textContent).toBe("R-024210");
    expect(host.querySelector(".mib-chart__iso-label")!.textContent).toBe("1.0");
  });

  describe("DataTable", () => {
    type Run = { id: string; n?: number; flag?: string };
    const columns = [
      { key: "id", header: "Run", emphasis: true },
      { key: "n", header: "n valid", numeric: true },
      { key: "flag", header: "Flags", cell: (r: Run) => (r.flag ? <StateChip tone="blocked" size="small">{r.flag}</StateChip> : null) },
    ];

    it("shows a value that was not recorded as missing, never as a number", () => {
      render(<DataTable columns={columns} rows={[{ id: "07" }]} rowKey={(r) => r.id} />);
      const cells = [...host.querySelectorAll<HTMLTableCellElement>("tbody td")];
      expect(cells.map((c) => c.textContent)).toEqual(["07", "–", "–"]);
      expect(cells[1].className).toContain("mib-table__cell--missing");
      expect(cells[1].className).toContain("mib-table__cell--numeric");
      expect(cells[1].title).toBe("Not recorded");
      expect(cells[0].className).toContain("mib-table__cell--emphasis");
    });

    it("renders an empty list as an empty cell, not as missing", () => {
      const listColumns = [{ key: "id", header: "Run" }, { key: "flags", header: "Flags", cell: () => [] as React.ReactNode[] }];
      render(<DataTable columns={listColumns} rows={[{ id: "01" }]} rowKey={(r) => r.id} />);
      const cell = host.querySelectorAll<HTMLTableCellElement>("tbody td")[1];
      expect(cell.textContent).toBe("");
      expect(cell.className).not.toContain("mib-table__cell--missing");
    });

    it("keeps a recorded zero as zero", () => {
      render(<DataTable columns={columns} rows={[{ id: "01", n: 0 }]} rowKey={(r) => r.id} />);
      expect(host.querySelectorAll("tbody td")[1].textContent).toBe("0");
    });

    it("says why when there are no rows", () => {
      render(<DataTable columns={columns} rows={[]} rowKey={(r: Run) => r.id} empty="No runs recorded yet." />);
      const cell = host.querySelector<HTMLTableCellElement>(".mib-table__empty")!;
      expect(cell.textContent).toBe("No runs recorded yet.");
      expect(cell.colSpan).toBe(3);
    });

    it("reports selection by click and keyboard with the string id", () => {
      const onSelect = vi.fn();
      const rows: Run[] = [{ id: "18446744073709551615", n: 1 }, { id: "2", n: 2 }];
      render(<DataTable columns={columns} rows={rows} rowKey={(r) => r.id} selectedKey="2" onSelect={onSelect} />);
      const trs = [...host.querySelectorAll<HTMLTableRowElement>("tbody tr")];
      expect(trs.map((tr) => tr.getAttribute("aria-selected"))).toEqual(["false", "true"]);
      expect(trs[1].className).toContain("mib-table__row--selected");
      act(() => trs[0].click());
      expect(onSelect).toHaveBeenLastCalledWith("18446744073709551615", rows[0]);
      act(() => trs[1].dispatchEvent(new KeyboardEvent("keydown", { key: "Enter", bubbles: true })));
      expect(onSelect).toHaveBeenLastCalledWith("2", rows[1]);
    });

    it("is not interactive without onSelect", () => {
      render(<DataTable columns={columns} rows={[{ id: "1" }]} rowKey={(r) => r.id} />);
      const tr = host.querySelector("tbody tr")!;
      expect(tr.hasAttribute("tabindex")).toBe(false);
      expect(tr.hasAttribute("aria-selected")).toBe(false);
    });
  });

  describe("CellImage", () => {
    const contour: Array<[number, number]> = [[10, 10], [50, 12], [52, 50], [12, 48]];

    it("says the image was not recorded instead of drawing a stand-in", () => {
      render(<CellImage recordId="R-1" imageWidth={64} imageHeight={64} contour={contour} />);
      expect(host.querySelector("img")).toBeNull();
      expect(host.querySelector("svg")).toBeNull();
      expect(host.querySelector(".mib-cell__missing")!.textContent).toBe("Image not recorded");
    });

    it("draws the recorded contour in image pixels", () => {
      render(<CellImage recordId="R-2" src="data:image/png;base64,AA==" imageWidth={64} imageHeight={48} contour={contour} />);
      const svg = host.querySelector("svg")!;
      expect(svg.getAttribute("viewBox")).toBe("0 0 64 48");
      expect(host.querySelector("polygon")!.getAttribute("points")).toBe("10,10 50,12 52,50 12,48");
      expect(host.querySelector(".mib-cell__overlay-missing")).toBeNull();
    });

    it("states a missing overlay with an unknown chip", () => {
      render(<CellImage recordId="R-3" src="data:image/png;base64,AA==" imageWidth={64} imageHeight={64} overlay="mask" />);
      const chip = host.querySelector(".mib-cell__overlay-missing")!;
      expect(chip.textContent).toBe("Mask not recorded");
      expect(chip.getAttribute("data-tone")).toBe("unknown");
    });

    it("pins a crop from the strip by its string id", () => {
      const onPin = vi.fn();
      const cells = ["18446744073709551615", "R-063002"].map((recordId) => ({ recordId, src: "data:image/png;base64,AA==", imageWidth: 64, imageHeight: 64 }));
      render(<CellStrip cells={cells} pinnedId="R-063002" onPin={onPin} />);
      const buttons = [...host.querySelectorAll<HTMLButtonElement>("button.mib-cell")];
      expect(buttons.map((b) => b.getAttribute("aria-pressed"))).toEqual(["false", "true"]);
      act(() => buttons[0].click());
      expect(onPin).toHaveBeenCalledWith("18446744073709551615");
    });

    it("says why an empty strip is empty", () => {
      render(<CellStrip cells={[]} empty="No cells detected yet in this run." />);
      expect(host.querySelector(".mib-cellstrip__empty")!.textContent).toBe("No cells detected yet in this run.");
    });
  });

  describe("OptionGroup", () => {
    const formats = [
      { value: "single", label: "Single sample", description: "One run, one file" },
      { value: "queue", label: "Sample queue" },
      { value: "rack", label: "Tube rack", unavailable: "Unsupported · no autosampler" },
    ];

    it("reports a single choice as a draft and keeps unavailable options visible but inert", () => {
      const onChange = vi.fn();
      render(<OptionGroup legend="Sample format" options={formats} value="queue" onChange={onChange} />);
      const inputs = [...host.querySelectorAll<HTMLInputElement>("input")];
      expect(inputs.map((i) => i.type)).toEqual(["radio", "radio", "radio"]);
      expect(inputs.map((i) => i.checked)).toEqual([false, true, false]);
      expect(inputs[2].disabled).toBe(true);
      expect(host.querySelector(".mib-option__reason")!.textContent).toBe("Unsupported · no autosampler");
      act(() => inputs[0].click());
      expect(onChange).toHaveBeenCalledWith("single");
      act(() => inputs[2].click());
      expect(onChange).toHaveBeenCalledTimes(1);
    });

    it("reports multiple choices in option order", () => {
      const onChange = vi.fn();
      const exports = [{ value: "csv", label: "Metrics CSV" }, { value: "fcs", label: "FCS" }, { value: "pdf", label: "Report" }];
      render(<OptionGroup type="multiple" legend="What to export" options={exports} value={["pdf"]} onChange={onChange} />);
      const inputs = [...host.querySelectorAll<HTMLInputElement>("input")];
      expect(inputs.map((i) => i.type)).toEqual(["checkbox", "checkbox", "checkbox"]);
      act(() => inputs[0].click());
      expect(onChange).toHaveBeenLastCalledWith(["csv", "pdf"]);
    });
  });

  describe("Notice", () => {
    it("is a status banner with the backend tone and its actions", () => {
      render(
        <Notice tone="blocked" title="Start blocked · 1 gate" actions={<Button>Focus setup</Button>}>
          Focus: coarse motor origin unknown.
        </Notice>,
      );
      const notice = host.querySelector(".mib-notice")!;
      expect(notice.getAttribute("role")).toBe("status");
      expect(notice.getAttribute("data-tone")).toBe("blocked");
      expect(notice.querySelector(".mib-notice__title")!.textContent).toBe("Start blocked · 1 gate");
      expect(notice.querySelector(".mib-notice__actions button")!.textContent).toBe("Focus setup");
    });
  });
});
