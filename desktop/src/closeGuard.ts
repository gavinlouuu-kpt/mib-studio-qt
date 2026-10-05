import {useEffect, useRef} from "react";
import {getCurrentWindow} from "@tauri-apps/api/window";
import {confirm} from "./transport/dialogs";
import {invoke} from "./transport";
import {bridge} from "./bridge";
import {isRemote} from "./transport";
import {EXPERIMENT_STATES} from "./bridgeContract";

// Both File→Exit and the OS close button use authoritative backend state.
// Never convert a close request into an implicit experiment stop/cancel.
export function useCloseGuard(options: {ready:boolean; busy:boolean; dirty:boolean; report:(text:string)=>void}) {
  const latest=useRef(options);latest.current=options;
  const pending=useRef(false), allowed=useRef(false);
  async function requestClose() {
    if(pending.current)return;
    pending.current=true;
    try {
      if(latest.current.busy)throw new Error("Finish or cancel pending work before closing.");
      if(latest.current.dirty && !await confirm("Discard unsaved configuration/profile drafts and close?",{title:"Unsaved changes",kind:"warning"}))return;
      if(latest.current.ready || await bridge.isInitialized()) {
        const [experiment,preview,exportJob,reanalysis,calibration]=await Promise.all([
          bridge.fetchExperimentStatus(),invoke<{capture_running:boolean;recording:boolean}>("fetch_preview_buffer"),
          bridge.reviewExportStatus(),bridge.reviewReanalysisStatus(),bridge.backgroundCalibrationStatus(),
        ]);
        if(!experiment.valid)throw new Error("Cannot verify experiment state; keep this window open and retry.");
        if([EXPERIMENT_STATES.Starting,EXPERIMENT_STATES.Active,EXPERIMENT_STATES.Stopping].includes(experiment.state as 1|2|3))
          throw new Error("Stop the experiment and wait for finalization before closing.");
        if(preview.recording)throw new Error("Stop raw recording before closing.");
        if([exportJob.state,reanalysis.state,calibration.state].includes("running"))
          throw new Error("Finish or cancel export, reanalysis or calibration before closing.");
        if(latest.current.busy)throw new Error("An operation started while checking; finish it before closing.");
        if(preview.capture_running) {const result=await bridge.stopCapture();if(!result.ok)throw new Error(result.message);}
      }
      allowed.current=true;
      try {if(isRemote)window.close();else await getCurrentWindow().close();}catch(e){allowed.current=false;throw e;}
    }catch(e){latest.current.report(`Close postponed: ${e}`);}
    finally{pending.current=false;}
  }
  const action=useRef(requestClose);action.current=requestClose;
  useEffect(()=>{
    // In a browser the instrument keeps running when a tab closes; the server stops and saves
    // only once no client is left (crates/mib-bridge-server). Warn before leaving mid-run.
    if(isRemote){
      const warn=(event:BeforeUnloadEvent)=>{if(latest.current.busy||latest.current.dirty){event.preventDefault();event.returnValue="";}};
      window.addEventListener("beforeunload",warn);
      return()=>window.removeEventListener("beforeunload",warn);
    }
    let disposed=false,cleanup:(()=>void)|undefined;
    void getCurrentWindow().onCloseRequested(event=>{
      if(allowed.current)return;
      event.preventDefault();void action.current();
    }).then(fn=>{if(disposed)fn();else cleanup=fn;}).catch(e=>latest.current.report(`Close guard unavailable: ${e}`));
    return()=>{disposed=true;cleanup?.();};
  },[]);
  return requestClose;
}
