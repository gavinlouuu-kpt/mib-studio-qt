import { expect,it } from "vitest";
import type { MonitoringRow } from "../bridge";
import { monitoringPlotData } from "./MonitoringCharts";
const row=(area:number,deformability:number,ring_ratio:number)=>({area,deformability,ring_ratio} as MonitoringRow);
it("bounds chart input and does not fabricate missing measurements",()=>{
  const data=monitoringPlotData(Array.from({length:300},(_,i)=>row(i,1,Number.NaN)));
  expect(data.scatter).toHaveLength(200);expect(data.scatter[0].area).toBe(100);
  expect(data.ratioCount).toBe(0);expect(data.omitted).toBe(100);
});
it("includes extrema and negative values without degenerate axes",()=>{
  const data=monitoringPlotData([row(-3,0,-2),row(5,0,2)]);
  expect(data.x).toEqual([-3,5]);expect(data.y[1]).toBeGreaterThan(data.y[0]);
  expect(data.bins[0]).toBe(1);expect(data.bins[9]).toBe(1);
  expect(data.bins.reduce((a,b)=>a+b,0)).toBe(2);
});
it("filters non-finite coordinate pairs independently from histogram samples",()=>{
  const data=monitoringPlotData([row(Infinity,1,1),row(2,NaN,1),row(3,4,Infinity)]);
  expect(data.scatter).toHaveLength(1);expect(data.invalid).toBe(2);expect(data.ratioCount).toBe(2);
  expect(data.range[1]).toBeGreaterThan(data.range[0]);
});
