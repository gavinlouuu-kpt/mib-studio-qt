// @vitest-environment jsdom
import { act } from "react";
import { createRoot } from "react-dom/client";
import { renderToStaticMarkup } from "react-dom/server";
import { describe, it, expect, vi } from "vitest";
import { ResultMetricCells } from "./components/ResultMetricCells";
import { PanelErrorBoundary } from "./components/PanelErrorBoundary";
import { FrameViewer } from "./review/FrameViewer";
import { METRIC_COLUMNS } from "./review/metricsColumns";
import { MonitoringCharts } from "./components/MonitoringCharts";
import { metricNumber } from "./metricFormat";
vi.mock("@tauri-apps/api/core", () => ({ invoke: vi.fn() }));
Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

describe("missing result metrics", () => {
  for (const value of [null, undefined, NaN, Infinity]) {
    it(`renders ${String(value)} in both result tables, review columns and frame viewer`, () => {
      const row = { area: value, area_um2: value, deformability: value, ring_ratio: value, youngs_modulus: value };
      const cells = renderToStaticMarkup(<table><tbody><tr><ResultMetricCells row={row} /></tr></tbody></table>);
      expect(cells.match(/—/g)).toHaveLength(4);
      const viewer = renderToStaticMarkup(<FrameViewer row={row as never} scheduler={{} as never} valid={false} index={0} total={1} overlay={0 as never} roiOverlay={false} onIndexChange={() => {}} onClose={() => {}} log={() => {}} />);
      expect(viewer.match(/—/g)).toHaveLength(5);
      for (const id of ["area", "area_um2", "deformability", "ring_ratio", "youngs_modulus"])
        expect(METRIC_COLUMNS.find(c => c.id === id)!.format(row as never)).toBe("—");
      expect(renderToStaticMarkup(<MonitoringCharts snapshot={{ valid: true, rows: [{ ...row, valid: true, pixel_to_micron: null }] } as never} />)).toContain("No calibrated finite");
      expect(metricNumber(value, 4, true)).toBe("—");
    });
  }
  it("preserves finite metric precision", () => {
    expect(metricNumber(1.23456, 3)).toBe("1.235");
    expect(metricNumber(1.23456, 4, true)).toBe("1.235");
  });
});

it("contains a thrown child and reloads the view while siblings remain mounted", async () => {
  const error = vi.spyOn(console, "error").mockImplementation(() => {});
  const host = document.createElement("div"), root = createRoot(host);
  let fail = true;
  function Child() { if (fail) throw new Error("bad metric"); return <p>Recovered view</p>; }
  try {
    await act(async () => root.render(<><button>Stop run</button><PanelErrorBoundary>{() => <Child />}</PanelErrorBoundary><aside>Log survives</aside></>));
    expect(host.querySelector('[role="alert"]')).not.toBeNull();
    expect(host.textContent).toContain("Stop run");
    expect(host.textContent).toContain("Log survives");
    fail = false;
    await act(async () => Array.from(host.querySelectorAll("button")).find(b => b.textContent === "Reload view")!.click());
    expect(host.textContent).toContain("Recovered view");
    expect(host.querySelector('[role="alert"]')).toBeNull();
  } finally { await act(async () => root.unmount()); error.mockRestore(); }
});

it("renders a PL-science row with only ring ratio absent", () => {
  const row = { area: 100, area_um2: 25, deformability: 0.2, ring_ratio: null, youngs_modulus: 2 };
  expect(renderToStaticMarkup(<table><tbody><tr><ResultMetricCells row={row} /></tr></tbody></table>)).toContain("<td>—</td>");
  expect(renderToStaticMarkup(<FrameViewer row={row as never} scheduler={{} as never} valid={false} index={0} total={1} overlay={0 as never} roiOverlay={false} onIndexChange={() => {}} onClose={() => {}} log={() => {}} />)).toContain("ratio —");
});

it("catches errors while evaluating panel markup", async () => {
  const error = vi.spyOn(console, "error").mockImplementation(() => {});
  const host = document.createElement("div"), root = createRoot(host);
  try {
    await act(async () => root.render(<PanelErrorBoundary>{() => { throw new Error("inline panel expression"); }}</PanelErrorBoundary>));
    expect(host.querySelector('[role="alert"]')).not.toBeNull();
  } finally { await act(async () => root.unmount()); error.mockRestore(); }
});
