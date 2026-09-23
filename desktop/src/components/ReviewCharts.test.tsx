// @vitest-environment jsdom
import {act} from "react";
import {createRoot,type Root} from "react-dom/client";
import {beforeEach,afterEach,it,expect,vi} from "vitest";
import {ReviewCharts} from "./ReviewCharts";
import {bridge} from "../bridge";
vi.mock("../bridge",()=>({bridge:{fetchReviewMetadata:vi.fn(),fetchReviewMetricsPage:vi.fn()}}));
Object.assign(globalThis,{IS_REACT_ACT_ENVIRONMENT:true});
let root:Root,host:HTMLDivElement;
beforeEach(()=>{vi.resetAllMocks();host=document.createElement("div");document.body.append(host);root=createRoot(host);vi.mocked(bridge.fetchReviewMetadata).mockResolvedValue({file_open:true,file_path:"a.h5"} as never);vi.mocked(bridge.fetchReviewMetricsPage).mockResolvedValue({valid:true,total:500,offset:0,rows:[]});});
afterEach(async()=>{await act(async()=>root.unmount());host.remove();});
it("reads bounded saved-file pages and labels subset rather than whole-file statistics",async()=>{
  await act(async()=>root.render(<ReviewCharts sourcePath="a.h5"/>));
  expect(bridge.fetchReviewMetricsPage).toHaveBeenCalledWith(true,0,200);
  expect(host.textContent).toContain("not whole-file statistics");
  const next=Array.from(host.querySelectorAll("button")).find(b=>b.textContent==="Next chart page")!;
  await act(async()=>next.click());expect(bridge.fetchReviewMetricsPage).toHaveBeenLastCalledWith(true,200,200);
});
it("rejects a metrics response after the native review source changes",async()=>{
  vi.mocked(bridge.fetchReviewMetadata).mockResolvedValueOnce({file_open:true,file_path:"a.h5"} as never).mockResolvedValue({file_open:true,file_path:"b.h5"} as never);
  await act(async()=>root.render(<ReviewCharts sourcePath="a.h5"/>));
  expect(host.querySelector('[role="alert"]')?.textContent).toContain("source changed");
  expect(host.querySelector("svg")).toBeNull();
});
it("switches class without carrying a previous page offset",async()=>{
  await act(async()=>root.render(<ReviewCharts sourcePath="a.h5"/>));
  await act(async()=>Array.from(host.querySelectorAll("button")).find(b=>b.textContent==="Next chart page")!.click());
  const select=host.querySelector("select")!;
  await act(async()=>{select.value="invalid";select.dispatchEvent(new Event("change",{bubbles:true}));});
  expect(bridge.fetchReviewMetricsPage).toHaveBeenLastCalledWith(false,0,200);
});
