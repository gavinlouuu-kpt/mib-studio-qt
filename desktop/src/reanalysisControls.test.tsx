// @vitest-environment jsdom
import {act} from "react";
import {createRoot,type Root} from "react-dom/client";
import {beforeEach,afterEach,it,expect,vi} from "vitest";
import {useReanalysis,ReanalysisStatus,ReanalysisControls,reanalysisStatusLine,reanalysisFactorLine} from "./reanalysisControls";
import {bridge,type ReviewMetadata} from "./bridge";
import {open,save} from "@tauri-apps/plugin-dialog";
vi.mock("./bridge",()=>({bridge:{reviewReanalysis:vi.fn(),reviewReanalysisStatus:vi.fn(),fetchReviewMetadata:vi.fn(),cancelOperation:vi.fn(),fetchReanalysisPreview:vi.fn(),fetchProcessingConfigJson:vi.fn(),fetchProcessingCoreStatus:vi.fn()},mono8ToImageData:vi.fn()}));
vi.mock("@tauri-apps/plugin-dialog",()=>({save:vi.fn(),open:vi.fn()}));
Object.assign(globalThis,{IS_REACT_ACT_ENVIRONMENT:true});
// jsdom has no canvas: the preview panel then draws nothing (it tolerates a missing 2d context).
Object.defineProperty(HTMLCanvasElement.prototype,"getContext",{value:()=>null,configurable:true});

const images=(count:number)=>({present:count>0,count,height:200,width:300,channels:1});
/** An experiment file (50 valid, 7 invalid frames of 300 x 200) that records ROI 10,20 100x80 and a background. */
const meta=(over:Partial<ReviewMetadata>={}):ReviewMetadata=>({
  valid:true,file_open:true,recording_file:false,start_time_ns:0,end_time_ns:0,total_valid:50,total_invalid:7,
  roi_x:10,roi_y:20,roi_w:100,roi_h:80,has_background:true,has_core_identity:false,core_version:"",core_source:"",core_release_tag:"",
  valid_images:images(50),invalid_images:images(7),valid_masks:images(0),invalid_masks:images(0),recorded_images:images(0),file_path:"/data/source.h5",...over,
}) as ReviewMetadata;
const BASIC={file_open:true,file_path:"source.h5"} as never;
const LIVE={gaussian_blur_size:5,bg_subtract_threshold:12,morph_kernel_size:7,morph_iterations:2,area_threshold_min:40,area_threshold_max:300,deformability_threshold_min:0.01,deformability_threshold_max:0.9};
const EIGHT=Object.keys(LIVE).sort();
const PACKET={valid:true,width:300,height:200,stride_bytes:300,data:new Uint8Array(0)} as never;

let showControls=false;
let metadata:ReviewMetadata|null=BASIC;
let openPath:ReturnType<typeof vi.fn>;
let root:Root,host:HTMLDivElement,model:ReturnType<typeof useReanalysis>;
function Harness(){model=useReanalysis(true,openPath as never);return <><ReanalysisStatus model={model}/>{showControls && <ReanalysisControls model={model} metadata={metadata}/>}</>;}
beforeEach(async()=>{
  vi.useFakeTimers();vi.resetAllMocks();showControls=false;metadata=BASIC;openPath=vi.fn().mockResolvedValue(true);
  host=document.createElement("div");document.body.append(host);root=createRoot(host);
  vi.mocked(save).mockResolvedValue("new.h5");
  vi.mocked(bridge.fetchReviewMetadata).mockResolvedValue({file_open:true,file_path:"source.h5"} as never);
  vi.mocked(bridge.reviewReanalysisStatus).mockResolvedValue({state:"idle"});
  vi.mocked(bridge.reviewReanalysis).mockResolvedValue({ok:true,operation_id:"12"} as never);
  vi.mocked(bridge.fetchProcessingConfigJson).mockResolvedValue({valid:true,json:JSON.stringify({image_processing:{...LIVE,threshold:17}})});
  vi.mocked(bridge.fetchReanalysisPreview).mockResolvedValue(PACKET);
  await act(async()=>root.render(<Harness/>));
});
afterEach(async()=>{await act(async()=>root.unmount());host.remove();vi.useRealTimers();});

const flush=async()=>{await act(async()=>{await vi.advanceTimersByTimeAsync(0);});};
const button=(text:string)=>Array.from(host.querySelectorAll("button")).find(b=>b.textContent===text) as HTMLButtonElement;
const field=(label:string)=>host.querySelector(`[aria-label="${label}"]`) as HTMLInputElement&HTMLSelectElement;
/** The control inside the <label> whose text contains `text` (a checkbox, a radio, a budget input). */
const labelled=(text:string)=>Array.from(host.querySelectorAll("label")).find(l=>l.textContent?.includes(text))!.querySelector("input,select") as HTMLInputElement&HTMLSelectElement;
async function click(element:HTMLElement){await act(async()=>{element.click();});await flush();}
async function setValue(element:HTMLInputElement|HTMLSelectElement,value:string){
  await act(async()=>{
    const select=element instanceof HTMLSelectElement;
    Object.getOwnPropertyDescriptor(select?HTMLSelectElement.prototype:HTMLInputElement.prototype,"value")!.set!.call(element,value);
    element.dispatchEvent(new Event(select?"change":"input",{bubbles:true}));
  });await flush();
}
/** Show the panel on `file` (its metadata is also what the pre-run source check reads). */
async function mount(file:ReviewMetadata=meta()){
  metadata=file;vi.mocked(bridge.fetchReviewMetadata).mockResolvedValue(file);showControls=true;
  await act(async()=>root.render(<Harness/>));await flush();await flush();
}
const runButton=()=>button("Regenerate into new HDF…");
async function run(){await click(runButton());await flush();}
const lastRequest=()=>{const calls=vi.mocked(bridge.reviewReanalysis).mock.calls;return calls[calls.length-1][0] as Record<string,unknown>;};

it("validates source again after choosing output and leaves source untouched",async()=>{
  vi.mocked(bridge.fetchReviewMetadata).mockResolvedValue({file_open:true,file_path:"other.h5"} as never);
  await act(async()=>model.start("source.h5","/valid_frames/images",0,10));
  expect(bridge.reviewReanalysis).not.toHaveBeenCalled();expect(host.textContent).toContain("source changed");
});
it("prevents duplicate submission and waits for native cancellation result",async()=>{
  await act(async()=>model.start("source.h5","/valid_frames/images",0,10));
  await act(async()=>model.start("source.h5","/valid_frames/images",0,10));
  expect(bridge.reviewReanalysis).toHaveBeenCalledOnce();
  vi.mocked(bridge.cancelOperation).mockResolvedValue({ok:true} as never);
  await act(async()=>model.cancel());expect(model.status.state).toBe("running");
  vi.mocked(bridge.reviewReanalysisStatus).mockResolvedValue({state:"cancelled",operation_id:"12"});
  await act(async()=>vi.advanceTimersByTimeAsync(500));expect(model.status.state).toBe("cancelled");
});
it("does not publish a failed backend job as a successful save",async()=>{
  await act(async()=>model.start("source.h5","/valid_frames/images",0,10));
  vi.mocked(bridge.reviewReanalysisStatus).mockResolvedValue({state:"failed",operation_id:"12",error:"Disk write failed"});
  await act(async()=>vi.advanceTimersByTimeAsync(500));expect(host.textContent).toContain("Disk write failed");expect(host.textContent).not.toContain("Output:");
});

it("accepts an independent image folder without replacing or requiring the open HDF reader",async()=>{
  vi.mocked(bridge.fetchReviewMetadata).mockClear();
  await act(async()=>model.start("/images","all",1,3,{source_kind:"folder",synthetic_background:true,roi:{x:0,y:0,w:10,h:10}}));
  expect(bridge.fetchReviewMetadata).not.toHaveBeenCalled();
  expect(bridge.reviewReanalysis).toHaveBeenCalledWith(expect.objectContaining({source_path:"/images",source_kind:"folder",start:1,count:3,synthetic_background:true}));
});

it("preserves a local processing-config edit across leaving and returning to Review",async()=>{
  await mount();
  expect(field("Area min").value).toBe("40");
  await setValue(field("Area min"),"75");
  showControls=false;await act(async()=>root.render(<Harness/>));
  showControls=true;await act(async()=>root.render(<Harness/>));await flush();
  expect(field("Area min").value).toBe("75");
  // "Reset to live defaults" re-reads the live pipeline.
  await click(button("Reset to live defaults"));
  expect(field("Area min").value).toBe("40");
});
it("does not offer the previous frame as background while the next one loads",async()=>{
  const waiting:Array<(value:never)=>void>=[];
  vi.mocked(bridge.fetchReanalysisPreview).mockImplementation(()=>new Promise(resolve=>{waiting.push(resolve as never);}));
  await mount();
  await act(async()=>{waiting[0](PACKET);});
  expect(button("Set as Background").disabled).toBe(false);          // frame 1 of the file is shown
  await click(button("Next →"));
  expect(waiting).toHaveLength(2);
  expect(button("Set as Background").disabled).toBe(true);           // frame 2 is on its way: frame 1 is not offered for it
  await act(async()=>{waiting[1](PACKET);});
  expect(button("Set as Background").disabled).toBe(false);
});
it("does not offer a late preview as background for a different requested index",async()=>{
  const waiting:Array<(value:never)=>void>=[];
  vi.mocked(bridge.fetchReanalysisPreview).mockImplementation(()=>new Promise(resolve=>{waiting.push(resolve as never);}));
  await mount();
  expect(waiting).toHaveLength(1);                                // preview 0, still in flight
  await click(labelled("Regenerate entire HDF5 file"));
  await setValue(field("Start (0-based)"),"1");
  expect(waiting).toHaveLength(2);                                // preview 1 requested; 0 is stale
  expect(button("Set as Background").disabled).toBe(true);
  await act(async()=>{waiting[0](PACKET);});                      // the late answer for 0
  expect(button("Set as Background").disabled).toBe(true);
  await act(async()=>{waiting[1](PACKET);});
  expect(button("Set as Background").disabled).toBe(false);
});
it("reload recovers a native reanalysis and blocks duplicate work until reconciliation",async()=>{
 await act(async()=>root.unmount());let finish!:(v:never)=>void;
 vi.mocked(bridge.reviewReanalysisStatus).mockImplementation(()=>new Promise(r=>{finish=r;}));root=createRoot(host);
 await act(async()=>root.render(<Harness/>));expect(model.busy).toBe(true);
 await act(async()=>model.start("source.h5","all",0,0));expect(bridge.reviewReanalysis).not.toHaveBeenCalled();
 await act(async()=>finish({state:"running",operation_id:"9007199254740993"} as never));
 vi.mocked(bridge.cancelOperation).mockResolvedValue({ok:true} as never);
 await act(async()=>model.cancel());expect(bridge.cancelOperation).toHaveBeenCalledWith("9007199254740993");
});

// ---- the Qt BatchMaskDialog's form (#867 REVIEW-057, -059..-062) -------------------------------

it("sends the eight config fields (and only those), the file's ROI, the saved background and Qt's synthetic default",async()=>{
  await mount();
  expect(host.textContent).toContain("Background: the file's saved background");
  expect(labelled("Use synthetic background").checked).toBe(true);
  expect(host.textContent).toContain("recorded in the file");
  await run();
  const request=lastRequest();
  expect(request).toMatchObject({source_path:"/data/source.h5",output_path:"new.h5",source_kind:"hdf",synthetic_background:true,roi:{x:10,y:20,w:100,h:80}});
  expect(Object.keys(request.image_processing as object).sort()).toEqual(EIGHT);
  expect(request.image_processing).toEqual(LIVE);          // the live pipeline's values; its other keys are not sent
  // the saved background: no background fields at all
  expect(request).not.toHaveProperty("clear_background");expect(request).not.toHaveProperty("background_index");expect(request).not.toHaveProperty("background_dataset");
});
it("sends an edited config value and the ROI the operator typed, and unticking the synthetic box",async()=>{
  await mount();
  await setValue(field("Area max"),"250");await setValue(field("ROI w"),"120");await click(labelled("Use synthetic background"));
  await run();
  expect(lastRequest()).toMatchObject({synthetic_background:false,roi:{x:10,y:20,w:120,h:80},image_processing:{...LIVE,area_threshold_max:250}});
  expect(host.textContent).not.toContain("recorded in the file");
});
it("maps Clear to a cleared background, and Use saved back to the file's own",async()=>{
  await mount();
  await click(button("Clear"));
  expect(host.textContent).toContain("Background: none (a synthetic one is built)");
  await click(button("Use saved"));
  expect(host.textContent).toContain("Background: the file's saved background");
  await click(button("Clear"));
  await run();
  expect(lastRequest()).toMatchObject({clear_background:true});expect(lastRequest()).not.toHaveProperty("background_index");
  expect(lastRequest()).not.toHaveProperty("background_dataset");
});
it("starts a file that records no background at none, without a Use saved button",async()=>{
  await mount(meta({has_background:false,file_path:"/data/plain.h5"}));
  expect(button("Use saved")).toBeUndefined();expect(host.textContent).toContain("Background: none (a synthetic one is built)");
  await run();
  expect(lastRequest()).toMatchObject({clear_background:true,synthetic_background:true});
});
it("sets the previewed frame, at its absolute source index and dataset, as the background",async()=>{
  await mount();
  await click(labelled("Regenerate entire HDF5 file"));
  await setValue(field("Range dataset"),"/invalid_frames/images");await setValue(field("Start (0-based)"),"3");
  await click(button("Next →"));
  expect(vi.mocked(bridge.fetchReanalysisPreview)).toHaveBeenLastCalledWith({source_kind:"hdf",source_path:"/data/source.h5",dataset:"/invalid_frames/images",index:4});
  expect(host.querySelector(".regen-frame-label")!.textContent).toBe("Frame 2 / 4");   // 7 invalid frames, 3.. = 4
  await click(button("Set as Background"));
  expect(host.textContent).toContain("Background: Frame 2");
  await run();
  expect(lastRequest()).toMatchObject({dataset:"/invalid_frames/images",start:3,count:100,clear_background:true,background_index:4,background_dataset:"/invalid_frames/images"});
});
it("regenerates the entire file by default, and a range with Qt's Start 0 / Count 100 once unticked",async()=>{
  await mount();
  expect(labelled("Regenerate entire HDF5 file").checked).toBe(true);
  expect(field("Start (0-based)").disabled).toBe(true);expect(field("Count (0 = to the end)").disabled).toBe(true);expect(field("Range dataset").disabled).toBe(true);
  await run();
  expect(lastRequest()).toMatchObject({dataset:"all",start:0,count:0});
  // the first job is still running: let it finish, then ask for a range
  vi.mocked(bridge.reviewReanalysisStatus).mockResolvedValue({state:"completed",operation_id:"12",final_path:"/data/a.h5"});
  await act(async()=>vi.advanceTimersByTimeAsync(500));
  await click(labelled("Regenerate entire HDF5 file"));
  expect(field("Start (0-based)").value).toBe("0");expect(field("Count (0 = to the end)").value).toBe("100");expect(field("Start (0-based)").disabled).toBe(false);
  expect(field("Range dataset").value).toBe("/valid_frames/images");
  await setValue(field("Start (0-based)"),"10");await setValue(field("Count (0 = to the end)"),"20");
  vi.mocked(bridge.reviewReanalysis).mockResolvedValue({ok:true,operation_id:"13"} as never);
  await run();
  expect(lastRequest()).toMatchObject({dataset:"/valid_frames/images",start:10,count:20});
});
it("steps the preview through the range only, and loads each frame by itself",async()=>{
  await mount();
  expect(vi.mocked(bridge.fetchReanalysisPreview)).toHaveBeenLastCalledWith({source_kind:"hdf",source_path:"/data/source.h5",dataset:"/valid_frames/images",index:0});
  expect(host.querySelector(".regen-frame-label")!.textContent).toBe("Frame 1 / 50");
  await click(labelled("Regenerate entire HDF5 file"));
  await setValue(field("Start (0-based)"),"5");await setValue(field("Count (0 = to the end)"),"3");
  expect(host.querySelector(".regen-frame-label")!.textContent).toBe("Frame 1 / 3");
  expect(vi.mocked(bridge.fetchReanalysisPreview)).toHaveBeenLastCalledWith({source_kind:"hdf",source_path:"/data/source.h5",dataset:"/valid_frames/images",index:5});
  await click(button("Next →"));await click(button("Next →"));
  expect(vi.mocked(bridge.fetchReanalysisPreview)).toHaveBeenLastCalledWith({source_kind:"hdf",source_path:"/data/source.h5",dataset:"/valid_frames/images",index:7});
  expect(button("Next →").disabled).toBe(true);                    // the end of the 3-frame range
});
it("starts a raw recording on its recorded dataset and the full frame",async()=>{
  await mount(meta({recording_file:true,has_background:false,roi_w:0,roi_h:0,valid_images:images(0),invalid_images:images(0),recorded_images:images(30),file_path:"/data/rec.h5"}));
  expect(vi.mocked(bridge.fetchReanalysisPreview)).toHaveBeenLastCalledWith({source_kind:"hdf",source_path:"/data/rec.h5",dataset:"/recorded_frames/images",index:0});
  expect(field("Range dataset").value).toBe("/recorded_frames/images");
  await run();
  expect(lastRequest()).not.toHaveProperty("roi");
  expect(lastRequest()).toMatchObject({clear_background:true,dataset:"all"});
});
it("Full frame beats the ROI the file records",async()=>{
  await mount();
  await click(button("Full frame"));
  await run();
  expect(lastRequest()).toMatchObject({roi:{x:0,y:0,w:300,h:200}});
});
it("refuses an ROI that reaches past the frame without calling the backend",async()=>{
  await mount();
  await setValue(field("ROI w"),"999");
  await run();
  expect(bridge.reviewReanalysis).not.toHaveBeenCalled();
  expect(host.textContent).toContain("The ROI reaches past the 300 × 200 frame");
});
it("regenerates from a folder: full frame, no saved background, the output inside the folder",async()=>{
  vi.mocked(open).mockResolvedValue("/data/frames/");
  await mount();
  await click(labelled("Folder of TIFF/PNG/JPEG images"));
  expect(host.querySelector(".regen-calibration")!.textContent).toBe("Folder and AVI sources record no px→µm factor: objects are validated at the live factor.");
  expect(button("Regenerate into new HDF…").disabled).toBe(true);   // no folder chosen yet
  await click(button("Browse…"));
  expect(field("Image folder").value).toBe("/data/frames/");
  expect(vi.mocked(bridge.fetchReanalysisPreview)).toHaveBeenLastCalledWith({source_kind:"folder",source_path:"/data/frames/",dataset:"all",index:0});
  expect(host.querySelector(".regen-frame-label")!.textContent).toBe("Frame 1");     // no frame count to show
  expect(button("Next →").disabled).toBe(false);
  expect(button("Use saved")).toBeUndefined();expect(host.textContent).toContain("Background: none");
  expect(field("ROI w").value).toBe("0");
  await run();
  expect(save).toHaveBeenCalledWith(expect.objectContaining({defaultPath:"/data/frames/frames_remasked.h5"}));
  expect(lastRequest()).toMatchObject({source_path:"/data/frames/",source_kind:"folder",dataset:"all",start:0,count:0,clear_background:true});
  expect(lastRequest()).not.toHaveProperty("roi");
});
it("proposes <base>_remasked.h5 beside an HDF5 or AVI source and inside an image folder",async()=>{
  await mount();
  await run();
  expect(save).toHaveBeenLastCalledWith(expect.objectContaining({defaultPath:"/data/source_remasked.h5"}));
  // the first job is done; an AVI source then proposes its name beside it
  vi.mocked(bridge.reviewReanalysisStatus).mockResolvedValue({state:"completed",operation_id:"12",final_path:"/data/a.h5"});
  await act(async()=>vi.advanceTimersByTimeAsync(500));
  vi.mocked(save).mockClear();
  await act(async()=>model.start("/data/rec/clip.avi","all",0,0,{source_kind:"avi"}));
  expect(save).toHaveBeenLastCalledWith(expect.objectContaining({defaultPath:"/data/rec/clip_remasked.h5"}));
});
it("defaults the input budget to the contract maxima and sends it",async()=>{
  await mount();
  expect(labelled("Maximum frames").value).toBe("1000000");expect(labelled("Input MiB").value).toBe("16384");
  expect(host.querySelector("details")!.textContent).not.toMatch(/4096|256 MiB/);
  await run();
  expect(lastRequest()).toMatchObject({max_frames:1000000,max_input_mib:16384});
});
it("refuses a budget outside the contract before running",async()=>{
  await mount();
  await setValue(labelled("Input MiB"),"16385");
  await run();
  expect(bridge.reviewReanalysis).not.toHaveBeenCalled();expect(host.textContent).toContain("1–16384 MiB");
});
it("starts again from the defaults when the open file changes",async()=>{
  await mount();
  await click(labelled("Regenerate entire HDF5 file"));await setValue(field("Count (0 = to the end)"),"7");await click(button("Clear"));await setValue(field("ROI w"),"50");
  await click(labelled("Folder of TIFF/PNG/JPEG images"));
  metadata=meta({file_path:"/data/next.h5",roi_x:1,roi_y:2,roi_w:30,roi_h:40});vi.mocked(bridge.fetchReviewMetadata).mockResolvedValue(metadata);
  await act(async()=>root.render(<Harness/>));await flush();
  expect(labelled("Current HDF5 frames").checked).toBe(true);
  expect(labelled("Regenerate entire HDF5 file").checked).toBe(true);expect(field("Count (0 = to the end)").value).toBe("100");
  expect(host.textContent).toContain("Background: the file's saved background");
  expect(field("ROI w").value).toBe("30");
  await run();
  expect(lastRequest()).toMatchObject({source_path:"/data/next.h5",roi:{x:1,y:2,w:30,h:40}});
});

// ---- the processing core (REVIEW-063) -----------------------------------------------------------

const withCore=(over:Partial<ReviewMetadata>={})=>meta({has_core_identity:true,core_version:"1.4.0",core_source:"bundled",core_release_tag:"v1.4.0",...over});
const activeCore=(over:Record<string,unknown>={})=>({valid:true,active_version:"1.4.0",contract_version:3,source:"bundled",release_tag:"v1.4.0",...over});
it("warns before running when the file's recorded core differs from the active one",async()=>{
  vi.mocked(bridge.fetchProcessingCoreStatus).mockResolvedValue(activeCore({active_version:"2.0.0",contract_version:4}) as never);
  await mount(withCore());
  const warning=host.querySelector(".regen-warning")!;
  expect(warning.getAttribute("role")).toBe("alert");
  expect(warning.textContent).toContain("recorded with processing core 1.4.0 (contract unknown)");
  expect(warning.textContent).toContain("currently active core 2.0.0 (contract 4)");
  expect(bridge.reviewReanalysis).not.toHaveBeenCalled();           // pre-run, not on Run
});
it("warns when only the core's source or release tag differs, and stays quiet on a match or when the status cannot be read",async()=>{
  vi.mocked(bridge.fetchProcessingCoreStatus).mockResolvedValue(activeCore({release_tag:"v1.4.1"}) as never);
  await mount(withCore());
  expect(host.querySelector(".regen-warning")).not.toBeNull();
  vi.mocked(bridge.fetchProcessingCoreStatus).mockResolvedValue(activeCore() as never);
  await mount(withCore({file_path:"/data/same.h5"}));
  expect(host.querySelector(".regen-warning")).toBeNull();
  vi.mocked(bridge.fetchProcessingCoreStatus).mockRejectedValue(new Error("no status"));
  await mount(withCore({file_path:"/data/third.h5"}));
  expect(host.querySelector(".regen-warning")).toBeNull();
});
it("does not look at the core of a file that records none",async()=>{
  await mount();
  expect(bridge.fetchProcessingCoreStatus).not.toHaveBeenCalled();expect(host.querySelector(".regen-warning")).toBeNull();
  expect(host.querySelector(".regen-calibration")!.textContent).toBe("Objects are validated at the file's recorded px→µm factor (the live factor when the file records none) and E with the source run's E-LUT.");
});

// ---- the status: factor and E (REVIEW-064), opening the new file (REVIEW-067) ---------------------

const COMPLETED={state:"completed",operation_id:"12",final_path:"/data/run-regen.h5",objects:"4",valid_objects:"3"} as const;
it("names the factor and the E-LUT a completed job validated with",async()=>{
  vi.mocked(bridge.reviewReanalysisStatus).mockResolvedValue({...COMPLETED,pixel_to_micron:0.5,pixel_to_micron_source:"recorded",emodulus_lut:"LUT A"} as never);
  await act(async()=>vi.advanceTimersByTimeAsync(500));
  expect(host.textContent).toContain("Validated at 0.5000 µm/px (recorded in the source file) · LUT A");
});
it("words the factor line for each source and leaves it out when the backend gives none",()=>{
  const done={state:"completed",final_path:"/x.h5"} as const;
  expect(reanalysisFactorLine({...done,pixel_to_micron:0.3333333,pixel_to_micron_source:"fallback"})).toBe("Validated at 0.3333 µm/px (live factor: the source records none)");
  expect(reanalysisFactorLine({...done,pixel_to_micron:1,pixel_to_micron_source:"operator",emodulus_lut:""})).toBe("Validated at 1.0000 µm/px (operator)");
  expect(reanalysisFactorLine(done)).toBe("");
  expect(reanalysisFactorLine({state:"running",pixel_to_micron:0.5})).toBe("");
});
it("opens the regenerated file in Review once, when a job started from this panel completes",async()=>{
  await act(async()=>model.start("source.h5","all",0,0));
  expect(openPath).not.toHaveBeenCalled();
  // every poll parses a new object, as the real bridge does
  vi.mocked(bridge.reviewReanalysisStatus).mockImplementation(async()=>({...COMPLETED}) as never);
  await act(async()=>vi.advanceTimersByTimeAsync(500));
  await act(async()=>vi.advanceTimersByTimeAsync(500));
  await act(async()=>vi.advanceTimersByTimeAsync(500));
  expect(openPath).toHaveBeenCalledTimes(1);expect(openPath).toHaveBeenCalledWith("/data/run-regen.h5");
  expect(host.textContent).toContain("Output: /data/run-regen.h5. Opened in Review.");
  expect(host.textContent).not.toContain("Open this file in Review");
});
it("does not open a file for a job this panel did not start, nor claim it was opened when the load failed",async()=>{
  vi.mocked(bridge.reviewReanalysisStatus).mockResolvedValue({...COMPLETED,operation_id:"99"} as never);   // recovered after a reload
  await act(async()=>vi.advanceTimersByTimeAsync(500));
  expect(openPath).not.toHaveBeenCalled();
  expect(host.textContent).toContain("Open this file in Review to inspect regenerated masks.");
  openPath.mockResolvedValue(false);
  vi.mocked(bridge.reviewReanalysisStatus).mockResolvedValue({state:"idle"});
  await act(async()=>vi.advanceTimersByTimeAsync(500));
  await act(async()=>model.start("source.h5","all",0,0));
  vi.mocked(bridge.reviewReanalysisStatus).mockResolvedValue(COMPLETED as never);
  await act(async()=>vi.advanceTimersByTimeAsync(500));
  expect(openPath).toHaveBeenCalledTimes(1);
  expect(host.textContent).toContain("Open this file in Review to inspect regenerated masks.");
});

// #763 F18: the terminal status has no phase/progress; the line said "Reanalysis: completed /".
it("reports a completed reanalysis with its records and output file, never an empty progress",async()=>{
  await act(async()=>model.start("source.h5","/valid_frames/images",0,10));
  vi.mocked(bridge.reviewReanalysisStatus).mockResolvedValue({state:"completed",operation_id:"12",final_path:"/data/run-regen.h5",objects:"45",valid_objects:"13",warnings:[],error:""} as never);
  await act(async()=>vi.advanceTimersByTimeAsync(500));
  const line=host.querySelector('[aria-label="Reanalysis status"] [role="status"]')!.textContent;
  expect(line).toBe("Reanalysis: completed · 45 records (13 valid, 32 invalid)");
  expect(line).not.toMatch(/\//);
  expect(host.textContent).toContain("Output: /data/run-regen.h5");
});
it("says so when a completed reanalysis names no output file",()=>{
  expect(reanalysisStatusLine({state:"completed",final_path:"",objects:"3"},false)).toBe("Reanalysis: completed · 3 records, but the backend named no output file");
  expect(reanalysisStatusLine({state:"completed"},false)).toBe("Reanalysis: completed, but the backend named no output file");
});
it("words every state without undefined fields",()=>{
  expect(reanalysisStatusLine({state:"running"},true)).toBe("Reanalysis: request pending");
  expect(reanalysisStatusLine({state:"running",phase:"loading"},false)).toBe("Reanalysis: running · loading the source");
  expect(reanalysisStatusLine({state:"running",phase:"processing",completed:"12",total:"57"},false)).toBe("Reanalysis: running · 12 of 57 frames processed");
  expect(reanalysisStatusLine({state:"running"},false)).toBe("Reanalysis: running");
  expect(reanalysisStatusLine({state:"completed",final_path:"/x.h5",objects:"45"},false)).toBe("Reanalysis: completed · 45 records");
  expect(reanalysisStatusLine({state:"cancelled"},false)).toBe("Reanalysis: cancelled");
  expect(reanalysisStatusLine({state:"failed",error:"Disk write failed"},false)).toBe("Reanalysis: failed");
  for(const state of ["idle","running","completed","cancelled","failed"] as const)expect(reanalysisStatusLine({state},false)).not.toMatch(/undefined| \/|\/$/);
});
