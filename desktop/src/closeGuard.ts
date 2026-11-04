import {useEffect, useRef} from "react";
import {getCurrentWindow} from "@tauri-apps/api/window";
import {confirm} from "./transport/dialogs";
import {invoke} from "./transport";
import {bridge} from "./bridge";
import {isRemote} from "./transport";
import {EXPERIMENT_STATES} from "./bridgeContract";

/** How long Close waits for the camera to stop before it asks the operator (#845). */
export const CLOSE_STOP_WAIT_MS = 10_000;

/**
 * Stop capture for Close with a bounded wait (#845). A camera on an external trigger with no
 * frames could take minutes to stop, and Close used to wait for all of it. Every
 * CLOSE_STOP_WAIT_MS without an answer, the operator chooses: close now (the backend finishes
 * the stop and shuts down on exit) or keep waiting, the dialog's default. Resolves when the
 * camera stopped or the operator chose to close; throws when the stop itself fails.
 */
export async function stopCaptureBounded(report:(text:string)=>void, waitMs=CLOSE_STOP_WAIT_MS):Promise<void> {
  report("Stopping camera…");
  const stop=bridge.stopCapture().then(result=>{if(!result.ok)throw new Error(result.message);return true as const;});
  stop.catch(()=>{}); // a forced close leaves this promise behind
  for(let waited=waitMs;;waited+=waitMs) {
    let timer:ReturnType<typeof setTimeout>|undefined;
    const timeout=new Promise<false>(resolve=>{timer=setTimeout(()=>resolve(false),waitMs);});
    try {if(await Promise.race([stop,timeout]))return;}
    finally {clearTimeout(timer);}
    const seconds=Math.round(waited/1000);
    report(`The camera has not stopped after ${seconds} s.`);
    if(await confirm(`The camera has not stopped after ${seconds} s.\n\nClose now (the backend finishes the stop as it shuts down), or keep waiting another ${Math.round(waitMs/1000)} s?`,
      {title:"Stopping camera",kind:"warning",okLabel:"Close now",cancelLabel:"Keep waiting"}))return;
    report("Still waiting for the camera to stop…");
  }
}

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
        if(preview.capture_running)await stopCaptureBounded(latest.current.report);
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
