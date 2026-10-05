// @vitest-environment jsdom
import {act} from "react";
import {createRoot,type Root} from "react-dom/client";
import {beforeEach,afterEach,it,expect,vi} from "vitest";
import {SavedReviewImage} from "./SavedReviewImage";
import {bridge,type ReviewMetadata} from "../bridge";
vi.mock("../bridge",()=>({bridge:{renderReviewOverlay:vi.fn()}}));
Object.assign(globalThis,{IS_REACT_ACT_ENVIRONMENT:true});
let root:Root,host:HTMLDivElement;
const metadata={file_path:"a.h5",valid_masks:{present:true},invalid_masks:{present:false},valid_images:{width:32},invalid_images:{width:32},roi_w:20,roi_h:20} as ReviewMetadata;
beforeEach(()=>{vi.resetAllMocks();host=document.createElement("div");document.body.append(host);root=createRoot(host);URL.createObjectURL=vi.fn(()=>"blob:rendered");URL.revokeObjectURL=vi.fn();vi.mocked(bridge.renderReviewOverlay).mockResolvedValue(new Uint8Array([137,80,78,71]));});
afterEach(async()=>{await act(async()=>root.unmount());host.remove();});
it("renders the selected saved source/index and classification-aware mask mode",async()=>{
  await act(async()=>root.render(<SavedReviewImage metadata={metadata} valid index={4}/>));
  const select=host.querySelector("select")!;
  await act(async()=>{select.value="3";select.dispatchEvent(new Event("change",{bubbles:true}));});
  expect(bridge.renderReviewOverlay).toHaveBeenLastCalledWith({source_path:"a.h5",valid:true,index:4,mode:3,roi:true});
  expect(host.querySelector("img")?.alt).toContain("saved image 5");
});
it("coalesces changing sources behind one native request and discards stale pixels",async()=>{
  let complete!:(value:Uint8Array<ArrayBuffer>)=>void;
  vi.mocked(bridge.renderReviewOverlay).mockReturnValueOnce(new Promise(resolve=>{complete=resolve;}));
  await act(async()=>root.render(<SavedReviewImage metadata={metadata} valid index={0}/>));
  await act(async()=>root.render(<SavedReviewImage metadata={{...metadata,file_path:"b.h5"}} valid index={1}/>));
  await act(async()=>root.render(<SavedReviewImage metadata={{...metadata,file_path:"c.h5"}} valid index={2}/>));
  expect(bridge.renderReviewOverlay).toHaveBeenCalledOnce();
  await act(async()=>complete(new Uint8Array([1])));
  expect(bridge.renderReviewOverlay).toHaveBeenCalledTimes(2);
  expect(bridge.renderReviewOverlay).toHaveBeenLastCalledWith(expect.objectContaining({source_path:"c.h5",index:2}));
  expect(URL.createObjectURL).toHaveBeenCalledOnce();
});
it("does not silently label an absent mask as an applied overlay",async()=>{
  await act(async()=>root.render(<SavedReviewImage metadata={metadata} valid={false} index={0}/>));
  expect(host.textContent).toContain("No saved mask");
  expect(host.querySelector<HTMLOptionElement>('option[value="3"]')?.disabled).toBe(true);
});
