// @vitest-environment jsdom
import {act} from "react";
import {createRoot,type Root} from "react-dom/client";
import {beforeEach,afterEach,it,expect,vi} from "vitest";
import {ReviewCharts} from "./ReviewCharts";
import {bridge,type ReviewChartSnapshot} from "../bridge";
vi.mock("../bridge",()=>({bridge:{fetchReviewCharts:vi.fn()}}));
Object.assign(globalThis,{IS_REACT_ACT_ENVIRONMENT:true});
let root:Root,host:HTMLDivElement;
const snapshot:ReviewChartSnapshot={valid:true,source_path:"a.h5",pixel_to_micron:0.5,rows:"50000",finite_points:"50000",excluded_nonfinite:"0",histogram_samples:"50000",area_range:[0,100],deform_range:[0,1],ring_range:[1,2],resolution:128,density:[[0,0,"25000"],[1,1,"25000"]],histogram:["20000","30000"],curves:[{modulus:1,points:[[1,0],[2,1]]}],curve_source:"reference conditions"};
beforeEach(()=>{vi.resetAllMocks();host=document.createElement("div");document.body.append(host);root=createRoot(host);vi.mocked(bridge.fetchReviewCharts).mockResolvedValue(snapshot);});
afterEach(async()=>{await act(async()=>root.unmount());host.remove();});
it("shows all-file aggregate counts with calibration and no subset claim",async()=>{
  await act(async()=>root.render(<ReviewCharts sourcePath="a.h5"/>));
  expect(bridge.fetchReviewCharts).toHaveBeenCalledOnce();expect(host.textContent).toContain("All 50000");expect(host.textContent).toContain("no sampling");expect(host.querySelectorAll("svg")).toHaveLength(2);
});
it("rejects a response for a different native source",async()=>{
  vi.mocked(bridge.fetchReviewCharts).mockResolvedValue({...snapshot,source_path:"b.h5"});
  await act(async()=>root.render(<ReviewCharts sourcePath="a.h5"/>));
  expect(host.querySelector('[role="alert"]')?.textContent).toContain("source changed");expect(host.querySelector("svg")).toBeNull();
});
it("can suppress reference overlays without dropping the full-data density",async()=>{
  await act(async()=>root.render(<ReviewCharts sourcePath="a.h5"/>));
  expect(host.querySelectorAll("polyline")).toHaveLength(1);
  await act(async()=>host.querySelector<HTMLInputElement>('input[type="checkbox"]')!.click());
  expect(host.querySelectorAll("polyline")).toHaveLength(0);expect(host.textContent).toContain("All 50000");
});
