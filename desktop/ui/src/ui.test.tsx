// @vitest-environment jsdom
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import {
  AppShell,
  AreaDeformChart,
  Button,
  Card,
  ConditionSwatch,
  FactList,
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
});
