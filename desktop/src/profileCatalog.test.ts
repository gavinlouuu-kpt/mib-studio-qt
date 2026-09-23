import {expect,it,vi} from "vitest";
import {invoke} from "@tauri-apps/api/core";
import {fetchProfileText,profileDifferences} from "./profileCatalog";
vi.mock("@tauri-apps/api/core",()=>({invoke:vi.fn()}));
it("diff includes nested additions removals and type changes without mutating inputs",()=>{const a={camera:{frame_delivery_mode:"everyFrame"},unknown:17};const b={camera:{frame_delivery_mode:"latestFrame"},new_key:true};expect(profileDifferences(a,b)).toEqual([{path:"camera.frame_delivery_mode",before:'"everyFrame"',after:'"latestFrame"'},{path:"new_key",before:"(absent)",after:"true"},{path:"unknown",before:"17",after:"(absent)"}]);expect(a.unknown).toBe(17);});
it("refuses local files and credential URLs before the native download command",async()=>{vi.mocked(invoke).mockClear();await expect(fetchProfileText("file:///tmp/config.json")).rejects.toThrow("HTTP");await expect(fetchProfileText("https://user:password@example.invalid/x")).rejects.toThrow("credentials");expect(invoke).not.toHaveBeenCalled();});
it("propagates bounded native transport failure without treating response as JSON",async()=>{vi.mocked(invoke).mockResolvedValue({ok:false,error:"too large"});await expect(fetchProfileText("https://example.invalid/config.json")).rejects.toThrow("too large");});
