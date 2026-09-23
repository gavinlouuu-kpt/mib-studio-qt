import {invoke} from "@tauri-apps/api/core";
import {bridge} from "./bridge";
export interface RuntimeFlags {capture_running:boolean;recording:boolean}
const defaultApi={isInitialized:bridge.isInitialized,init:()=>bridge.init(""),runtime:()=>invoke<RuntimeFlags>("fetch_preview_buffer"),experiment:bridge.fetchExperimentStatus,review:bridge.fetchReviewMetadata};
// Publish readiness only after authoritative native state is reconciled. A retained
// backend is a resumed session, even when currently idle: never replay startup config.
export async function recoverNativeRuntime(api=defaultApi){
 const resumed=await api.isInitialized();
 if(!resumed&&!await api.init())throw new Error("backend initialization failed");
 const [runtime,experiment,review]=await Promise.all([api.runtime(),api.experiment(),api.review()]);
 return {resumed,runtime,experiment,review};
}
