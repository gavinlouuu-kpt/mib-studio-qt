// @vitest-environment jsdom
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { beforeEach, afterEach, expect, it, vi } from "vitest";
import { bridge } from "./bridge";
import { useReviewExport, ExportStatus } from "./exportControls";
import { open, save } from "@tauri-apps/plugin-dialog";
import { describeExportFile } from "./reviewExport";
vi.mock("./bridge",()=>({bridge:{fetchReviewMetadata:vi.fn(),reviewExport:vi.fn(),reviewExportStatus:vi.fn(),cancelOperation:vi.fn()}}));
vi.mock("@tauri-apps/plugin-dialog",()=>({open:vi.fn(),save:vi.fn()}));
Object.assign(globalThis,{IS_REACT_ACT_ENVIRONMENT:true});
let root:Root,host:HTMLDivElement, model:ReturnType<typeof useReviewExport>, visible:boolean;
function Harness(){model=useReviewExport(true,vi.fn());return visible ? <ExportStatus model={model}/> : <p>Other tab</p>;}
const running={state:"running" as const,operation_id:"9007199254740993",phase:"valid_images",completed:"3",total:"12"};
beforeEach(async()=>{
  vi.useFakeTimers();vi.resetAllMocks();visible=true;
  const saved=new Map<string,string>();
  vi.stubGlobal("localStorage",{getItem:(k:string)=>saved.get(k)??null,setItem:(k:string,v:string)=>void saved.set(k,String(v))});
  vi.mocked(bridge.fetchReviewMetadata).mockResolvedValue({file_open:true,file_path:"/tmp/source-a.h5"} as never);
  vi.mocked(open).mockResolvedValue("/tmp/export");
  vi.mocked(bridge.reviewExportStatus).mockResolvedValue({state:"idle"});
  vi.mocked(bridge.reviewExport).mockResolvedValue({ok:true,operation_id:running.operation_id,message:"accepted"} as never);
  vi.mocked(bridge.cancelOperation).mockResolvedValue({ok:true,message:"cancel requested"} as never);
  host=document.createElement("div");document.body.append(host);root=createRoot(host);
  await act(async()=>root.render(<Harness/>));
});
afterEach(async()=>{await act(async()=>root.unmount());host.remove();vi.useRealTimers();vi.unstubAllGlobals();});
it("does not treat cancellation acceptance as completed cancellation",async()=>{
  vi.mocked(bridge.reviewExportStatus).mockResolvedValue(running);
  await act(async()=>{await model.start("all",0.5);});
  await act(async()=>{await model.cancel();});
  expect(bridge.cancelOperation).toHaveBeenCalledWith("9007199254740993");
  expect(model.status.state).toBe("running");
  vi.mocked(bridge.reviewExportStatus).mockResolvedValue({...running,state:"cancelled",retained_partial_path:"/tmp/.run.partial-1"});
  await act(async()=>{await vi.advanceTimersByTimeAsync(500);});
  expect(host.textContent).toContain("Incomplete output retained: /tmp/.run.partial-1");
});
it("retains and reconciles a native job when navigating away",async()=>{
  vi.mocked(bridge.reviewExportStatus).mockResolvedValue(running);
  await act(async()=>{await model.start("images",0.5);});
  visible=false;await act(async()=>root.render(<Harness/>));
  vi.mocked(bridge.reviewExportStatus).mockResolvedValue({...running,state:"completed",final_path:"/tmp/export/run"});
  await act(async()=>{await vi.advanceTimersByTimeAsync(500);});
  visible=true;await act(async()=>root.render(<Harness/>));
  expect(model.status.state).toBe("completed");expect(host.textContent).toContain("/tmp/export/run");
  expect(bridge.cancelOperation).not.toHaveBeenCalled();
});
it("bounds pending submissions and never clears command rejection on a good status poll",async()=>{
  let reject!:(reason:Error)=>void;
  vi.mocked(bridge.reviewExport).mockReturnValue(new Promise((_,r)=>{reject=r;}));
  let command!:Promise<void>;
  await act(async()=>{command=model.start("all",1);await Promise.resolve();});
  await act(async()=>{await model.start("all",1);});
  expect(bridge.reviewExport).toHaveBeenCalledOnce();
  await act(async()=>{reject(new Error("Export destination is invalid"));await command;});
  await act(async()=>{await vi.advanceTimersByTimeAsync(500);});
  expect(host.textContent).toContain("Export destination is invalid");
});
it("never overlaps status polls under a slow native response",async()=>{
  vi.mocked(bridge.reviewExportStatus).mockClear();
  let finish!:(value:typeof running)=>void;
  vi.mocked(bridge.reviewExportStatus).mockReturnValue(new Promise(r=>{finish=r;}));
  await act(async()=>{await vi.advanceTimersByTimeAsync(5000);});
  expect(bridge.reviewExportStatus).toHaveBeenCalledOnce();
  await act(async()=>{finish(running);});
});

it("submits batch sources once without replacing the open review file",async()=>{
  vi.mocked(open).mockResolvedValueOnce(["/data/a.h5","/data/b.h5"]).mockResolvedValueOnce("/tmp/out");
  await act(async()=>{await model.start("metrics_csv",undefined,true);});
  expect(bridge.reviewExport).toHaveBeenCalledWith(expect.objectContaining({source_paths:["/data/a.h5","/data/b.h5"],output_root:"/tmp/out",format:"metrics_csv"}));
  expect(vi.mocked(bridge.reviewExport).mock.calls[0][0]).not.toHaveProperty("explicit_destination");
});
it("cancelled batch selection submits nothing",async()=>{
  vi.mocked(open).mockResolvedValueOnce(null);
  await act(async()=>{await model.start("all",undefined,true);});
  expect(bridge.reviewExport).not.toHaveBeenCalled();
});

it("an idle poll started before acceptance cannot unlock an accepted export",async()=>{
  let complete!:(status:{state:"idle"})=>void;
  vi.mocked(bridge.reviewExportStatus).mockReturnValueOnce(new Promise(resolve=>{complete=resolve;}));
  await act(async()=>{await vi.advanceTimersByTimeAsync(500);});
  await act(async()=>{await model.start("all",undefined);});
  expect(model.status.state).toBe("running");
  await act(async()=>{complete({state:"idle"});});
  expect(model.status.state).toBe("running");
  await act(async()=>{await model.start("all",undefined);});
  expect(bridge.reviewExport).toHaveBeenCalledOnce();
});

it("passes explicit frame and series options to the native exporter",async()=>{
  await act(async()=>{model.options.setFrames("valid");model.options.setSeriesStart("2");model.options.setSeriesEnd("5");model.options.setIsoelastic(false);});
  await act(async()=>model.start("all"));
  expect(bridge.reviewExport).toHaveBeenCalledWith(expect.objectContaining({frames:"valid",series:{enabled:true,start:2,end:5},isoelastic_overlays:false}));
});
it("rejects reversed series ranges without starting an export",async()=>{
  await act(async()=>{model.options.setSeriesStart("5");model.options.setSeriesEnd("2");});
  await act(async()=>model.start("images"));expect(bridge.reviewExport).not.toHaveBeenCalled();
});
it("reload reconciles an existing native export before allowing another submission",async()=>{
 await act(async()=>root.unmount());let finish!:(v:typeof running)=>void;
 vi.mocked(bridge.reviewExportStatus).mockImplementation(()=>new Promise(r=>{finish=r;}));root=createRoot(host);
 await act(async()=>root.render(<Harness/>));expect(model.busy).toBe(true);
 await act(async()=>model.start("images"));expect(bridge.reviewExport).not.toHaveBeenCalled();
 await act(async()=>finish(running));expect(model.status.operation_id).toBe(running.operation_id);
 vi.mocked(bridge.reviewExportStatus).mockResolvedValue(running);
 await act(async()=>model.cancel());expect(bridge.cancelOperation).toHaveBeenCalledWith(running.operation_id);
});

it("pins the source before the destination picker can switch review files",async()=>{vi.mocked(open).mockImplementation(async()=>{vi.mocked(bridge.fetchReviewMetadata).mockResolvedValue({file_open:true,file_path:"/tmp/source-b.h5"} as never);return "/tmp/export";});await act(async()=>model.start("all"));expect(bridge.reviewExport).toHaveBeenCalledWith(expect.objectContaining({source_paths:["/tmp/source-a.h5"]}));});

// #867: FCS, each file at its recorded px→µm factor (the live one only as the fallback, #854),
// the Qt dialogs and the remembered folder.
it("exports FCS of the open file at its recorded factor through the Qt save dialog",async()=>{
  vi.mocked(save).mockResolvedValue("/tmp/out/cells");
  await act(async()=>{await model.start("fcs",0.4886);});
  expect(save).toHaveBeenCalledWith(expect.objectContaining({title:"Export FCS 3.1",defaultPath:"/tmp/source-a.fcs",
    filters:[{name:"FCS 3.1 Files",extensions:["fcs"]},{name:"All Files",extensions:["*"]}]}));
  expect(bridge.reviewExport).toHaveBeenCalledWith(expect.objectContaining({format:"fcs",output_root:"/tmp/out",
    explicit_destination:"/tmp/out/cells.fcs",source_paths:["/tmp/source-a.h5"],conversion_factor_source:"recorded",
    conversion_factor:0.4886,keep_partial_on_failure:false}));
  expect(vi.mocked(bridge.reviewExport).mock.calls[0][0]).not.toHaveProperty("refuse_recordings");
});
it("batch FCS asks for the files, then the folder, and refuses recordings",async()=>{
  vi.mocked(open).mockResolvedValueOnce(["/data/a.h5","/data/b.hdf5"]).mockResolvedValueOnce("/data/fcs");
  await act(async()=>{await model.start("fcs",0.4886,true);});
  expect(vi.mocked(open).mock.calls[0][0]).toMatchObject({title:"Select HDF Files for Batch FCS Export",multiple:true,
    filters:[{name:"HDF5 Files",extensions:["h5","hdf5"]},{name:"All Files",extensions:["*"]}]});
  expect(vi.mocked(open).mock.calls[1][0]).toMatchObject({title:"Select Directory for FCS Files",directory:true});
  const request=vi.mocked(bridge.reviewExport).mock.calls[0][0];
  expect(request).toMatchObject({format:"fcs",output_root:"/data/fcs",source_paths:["/data/a.h5","/data/b.hdf5"],
    refuse_recordings:true,conversion_factor_source:"recorded"});
  expect(request).not.toHaveProperty("explicit_destination");
});
it("batch metrics refuses recordings and uses each file's factor",async()=>{
  vi.mocked(open).mockResolvedValueOnce(["/data/a.h5"]).mockResolvedValueOnce("/data/csv");
  await act(async()=>{await model.start("metrics_csv",0.4886,true);});
  expect(vi.mocked(open).mock.calls[1][0]).toMatchObject({title:"Select Directory for Metrics CSV Files"});
  expect(bridge.reviewExport).toHaveBeenCalledWith(expect.objectContaining({format:"metrics_csv",refuse_recordings:true,conversion_factor_source:"recorded"}));
});
it("refuses metrics and FCS of a raw recording before any dialog",async()=>{
  vi.mocked(bridge.fetchReviewMetadata).mockResolvedValue({file_open:true,file_path:"/tmp/raw.h5",recording_file:true} as never);
  for(const format of ["metrics_csv","fcs"] as const){await act(async()=>{await model.start(format,0.5);});}
  expect(save).not.toHaveBeenCalled();expect(bridge.reviewExport).not.toHaveBeenCalled();
  expect(model.error).toContain("Recording files carry no object metrics");
});
it("proposes <base>_metrics.csv in the folder of the last successful export",async()=>{
  vi.mocked(save).mockResolvedValue("/tmp/picked/run_metrics.csv");
  vi.mocked(bridge.reviewExportStatus).mockResolvedValue(running);
  await act(async()=>{await model.start("metrics_csv",0.5);});
  expect(save).toHaveBeenLastCalledWith(expect.objectContaining({title:"Export Metrics to CSV",defaultPath:"/tmp/source-a_metrics.csv"}));
  vi.mocked(bridge.reviewExportStatus).mockResolvedValue({...running,state:"completed",final_path:"/tmp/picked/run_metrics.csv",
    results:[{source_path:"/tmp/source-a.h5",state:"completed",final_path:"/tmp/picked/run_metrics.csv",retained_partial_path:"",error:"",
      conversion_factor:0.25,conversion_factor_source:"recorded",valid_count:"6",invalid_count:"2"}],file_count:1});
  await act(async()=>{await vi.advanceTimersByTimeAsync(500);});
  expect(host.textContent).toContain("px→µm 0.25 (recorded in the file)");
  await act(async()=>{await model.start("metrics_csv",0.5);});
  expect(save).toHaveBeenLastCalledWith(expect.objectContaining({defaultPath:"/tmp/picked/source-a_metrics.csv"}));
});
it("describes an FCS result with its counts and event map",()=>{
  expect(describeExportFile({source_path:"/a.h5",state:"completed",final_path:"/o/a.fcs",retained_partial_path:"",error:"",
    conversion_factor:0.4886,conversion_factor_source:"fallback",valid_count:"5",invalid_count:"0"}))
    .toBe("px→µm 0.4886 (live factor: the file records none) · 5 FCS 3.1 events (5 valid, 0 invalid) · event map /o/a_event_map.csv");
  expect(describeExportFile({source_path:"/a.h5",state:"completed",final_path:"/o/a",retained_partial_path:"",error:""})).toBe("");
});
