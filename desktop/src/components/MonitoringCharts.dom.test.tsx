// @vitest-environment jsdom
import {act} from "react";
import {createRoot} from "react-dom/client";
import {it,expect,vi} from "vitest";
import {invoke} from "@tauri-apps/api/core";
import {MonitoringCharts} from "./MonitoringCharts";
vi.mock("@tauri-apps/api/core",()=>({invoke:vi.fn()}));
Object.assign(globalThis,{IS_REACT_ACT_ENVIRONMENT:true});
it("shows optional shared reference curves only on calibrated axes with explicit conditions",async()=>{
 vi.mocked(invoke).mockResolvedValue(JSON.stringify({curves:[{modulus:2,points:[[25,0.1],[400,0.3]]}],curve_source:"Bundled reference, channel width 30 um"}));
 const host=document.createElement("div"),root=createRoot(host);
 await act(async()=>root.render(<MonitoringCharts snapshot={{valid:true,rows:[{valid:true,area:100,deformability:0.2,pixel_to_micron:0.5,ring_ratio:2,youngs_modulus:2},{valid:true,area:100,deformability:0.3,pixel_to_micron:2,ring_ratio:2,youngs_modulus:2},{valid:true,area:100,deformability:0.3,pixel_to_micron:0,ring_ratio:2,youngs_modulus:2}]} as never}/>));
 expect(host.textContent).toContain("1 valid rows lack analysis-time calibration");
 expect(host.querySelectorAll("polyline")).toHaveLength(0);
 await act(async()=>host.querySelector<HTMLInputElement>('input[type="checkbox"]')!.click());
 expect(host.querySelectorAll("polyline")).toHaveLength(1);expect(host.textContent).toContain("channel width 30 um");
 expect(invoke).toHaveBeenCalledWith("fetch_monitoring_chart_reference");
 await act(async()=>root.unmount());
});
