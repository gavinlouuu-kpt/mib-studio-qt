// Configuration (epic #864): the live config document and the quick processing controls, processing
// cores, profiles, the checked config document, and the camera script and camera documents.
// Moved from App.tsx without change (#869 wave 0); studio/useStudioController.ts calls these hooks
// in App.tsx's original effect order.
import { useCallback, useEffect, useRef, useState } from "react";
import { bridge, type ProcessingCoreStatus, type ProcessingStats } from "../../bridge";
import { useCameraDocument } from "../../cameraDocument";
import { useCameraScript } from "../../cameraScript";
import { useConfigDocument } from "../../configDocument";
import { useCoreManagement } from "../../coreManagement";
import { useLiveConfigDraft } from "../../liveConfigDraft";
import { useProfiles } from "../../profiles";
import type { CameraConnectionModel, ConnectState } from "../connect/useConnect";
import type { HardwareState } from "../hardware/useHardware";
import type { MonitoringState } from "../monitoring/useMonitoring";
import type { PreviewState } from "../preview/usePreview";
import type { ShellState } from "../shell/useShell";

export function useConfigState() {
  const [configTab, setConfigTab] = useState<"app" | "script">("app");
  // Processing (bridge schema v3).
  const quickDraft = useLiveConfigDraft();
  const {acceptRemote: acceptQuickConfig, generation: quickGeneration, applied: quickApplied} = quickDraft;
  const quickValues = quickDraft.text ? JSON.parse(quickDraft.text) : {enabled:false,factor:"1.0"};
  const procEnabled = Boolean(quickValues.enabled);
  const pixelToMicron = String(quickValues.factor);
  const [stats, setStats] = useState<ProcessingStats | null>(null);

  // Processing config / ROI / background / core identity (schema v8, BE-3).
  const liveDraft=useLiveConfigDraft();
  const {text:configText,dirty:configDirty,acceptRemote:acceptConfig,applied:configApplied,generation:configGeneration}=liveDraft;
  const [coreStatus, setCoreStatus] = useState<ProcessingCoreStatus | null>(null);
  const procEnabledRef = useRef(procEnabled);
  procEnabledRef.current = procEnabled;
  return {
    configTab, setConfigTab, quickDraft, acceptQuickConfig, quickGeneration, quickApplied, procEnabled, pixelToMicron,
    stats, setStats, liveDraft, configText, configDirty, acceptConfig, configApplied, configGeneration,
    coreStatus, setCoreStatus,
  };
}
export type ConfigState = ReturnType<typeof useConfigState>;

type ConfigSyncDeps = Pick<ShellState, "ready" | "append">
  & Pick<ConfigState, "configGeneration" | "quickGeneration" | "acceptConfig" | "acceptQuickConfig" | "setCoreStatus" | "liveDraft"
    | "configText" | "configApplied" | "quickDraft" | "quickApplied" | "procEnabled" | "pixelToMicron" | "setStats">
  & Pick<PreviewState, "setBackgroundSet" | "setAutoBackgroundEnabled" | "setRoiFields"> & Pick<MonitoringState, "setBackgroundRevision">;

// The live config: fetched when the backend is ready, merge-applied, and the quick processing controls.
export function useConfigSync({
  ready, append, configGeneration, quickGeneration, acceptConfig, acceptQuickConfig, setCoreStatus, liveDraft, configText,
  configApplied, quickDraft, quickApplied, procEnabled, pixelToMicron, setStats, setBackgroundSet, setAutoBackgroundEnabled,
  setRoiFields, setBackgroundRevision,
}: ConfigSyncDeps) {
  const refreshConfig = useCallback(async (discardDraft=false) => {
    const generation=configGeneration();
    const quickToken=quickGeneration();
    try {
      const doc = await bridge.fetchProcessingConfigJson();
      if (doc.valid) {
        const parsed = JSON.parse(doc.json);
        acceptConfig(JSON.stringify(parsed, null, 2),discardDraft,generation);
        setBackgroundSet(Boolean(parsed.background_set));
        // The side panel's thumbnail refetches when the config was re-read with a background (set again, or replaced): epic #866 MON-102.
        if (parsed.background_set) setBackgroundRevision((n) => n + 1);
        setAutoBackgroundEnabled(Boolean(parsed.image_processing?.auto_background_enabled));
        acceptQuickConfig(JSON.stringify({enabled:Boolean(parsed.realtime_processing?.enabled),factor:String(parsed.pixel_to_micron ?? 1)}),discardDraft,quickToken);
        if (parsed.roi) {
          setRoiFields({
            x: String(parsed.roi.x ?? 0),
            y: String(parsed.roi.y ?? 0),
            w: String(parsed.roi.w ?? 0),
            h: String(parsed.roi.h ?? 0),
          });
        }
      }
      setCoreStatus(await bridge.fetchProcessingCoreStatus());
    } catch (e) {
      append(`config fetch error: ${e}`);
    }
  }, [append,acceptConfig,configGeneration,acceptQuickConfig,quickGeneration]);

  useEffect(() => {
    if (ready) void refreshConfig();
  }, [ready, refreshConfig]);

  const onApplyConfigJson = useCallback(async () => {
    if (!liveDraft.canApply()) return append("Runtime configuration changed. Reload and reconcile the preserved draft before applying.");
    try {
      const res = await bridge.applyProcessingConfigJson(configText);
      if (!res.ok) return append(`config apply failed: ${res.message}`);
      append("processing config applied");
      configApplied(configText);
      await refreshConfig();
    } catch (e) {
      append(`config apply error: ${e}`);
    }
  }, [configText, append, refreshConfig,configApplied,liveDraft.canApply]);

  // ---- Processing settings (bridged subset of App config) ----

  const onApplyProcessing = useCallback(async () => {
    if (!quickDraft.canApply()) return append("Runtime processing controls changed. Reload and reconcile the preserved draft before applying.");
    try {
      const factor = Number(pixelToMicron);
      if (!pixelToMicron.trim() || !Number.isFinite(factor) || factor <= 0) return append("processing failed: px→µm must be a finite positive number");
      const submitted=quickDraft.text;
      const res = await bridge.applyProcessing(procEnabled, factor);
      if (!res.ok) return append(`processing failed: ${res.message}`);
      append(`processing ${procEnabled ? "enabled" : "disabled"} (px→µm ${factor})`);
      quickApplied(submitted);
      await refreshConfig();
      setStats(await bridge.fetchProcessingStats());
    } catch (e) {
      append(`processing error: ${e}`);
    }
  }, [procEnabled, pixelToMicron, append, quickDraft.text, quickDraft.canApply, quickApplied, refreshConfig]);
  return { refreshConfig, onApplyConfigJson, onApplyProcessing };
}
export type ConfigSyncModel = ReturnType<typeof useConfigSync>;

type CameraSetupDeps = Pick<ShellState, "ready" | "append" | "instrument"> & Pick<ConnectState, "running" | "camSelection" | "runFpsStatus">
  & Pick<MonitoringState, "expActive"> & Pick<CameraConnectionModel, "refreshCameraState">
  & Pick<HardwareState, "operatingMode" | "triggerArmed">;

// The eGrabber camera script and the camera documents (script, MindVision, PZ7035).
export function useCameraSetup({
  ready, append, instrument, running, camSelection, runFpsStatus, expActive, refreshCameraState, operatingMode, triggerArmed,
}: CameraSetupDeps) {
  const cameraScript = useCameraScript({
    ready, running, experimentActive: expActive, selection: camSelection, append,
    refresh: refreshCameraState,
  });
  const cameraDocumentContext={ready,running,experimentActive:expActive,selection:camSelection,append,refresh:refreshCameraState};
  const scriptDocument=useCameraDocument(cameraDocumentContext,"js");
  const mindvisionDocument=useCameraDocument(cameraDocumentContext,"json",operatingMode==="service"&&triggerArmed);
  const pzCameraDocument=useCameraDocument(cameraDocumentContext,"pz7035",false,{status:runFpsStatus,ringFrames:instrument?.ring?.count??0});
  return { cameraScript, scriptDocument, mindvisionDocument, pzCameraDocument };
}
export type CameraSetupModel = ReturnType<typeof useCameraSetup>;

type CoresDeps = Pick<ShellState, "ready" | "resumedNative" | "append"> & Pick<MonitoringState, "expActive">
  & Pick<ConfigSyncModel, "refreshConfig">;

export function useCores({ ready, resumedNative, append, expActive, refreshConfig }: CoresDeps) {
  const cores = useCoreManagement({ready,resume:resumedNative,active:expActive,append,onChanged:refreshConfig});
  return { cores };
}
export type CoresModel = ReturnType<typeof useCores>;

type ConfigDocumentsDeps = Pick<ShellState, "ready" | "resumedNative" | "append"> & Pick<MonitoringState, "expActive">
  & Pick<ConfigSyncModel, "refreshConfig"> & CoresModel & Pick<PreviewState, "setPreviewFpsLimit">;

// The checked config document and the profiles (a profile's display rate paces the preview).
export function useConfigDocuments({
  ready, resumedNative, append, expActive, refreshConfig, cores, setPreviewFpsLimit,
}: ConfigDocumentsDeps) {
  const checkedConfig = useConfigDocument({ready, active:expActive, append, refresh:refreshConfig});
  const profiles = useProfiles({currentConfig:checkedConfig.doc?.document_json??null, ready:ready && cores.initialized, resume:resumedNative, active:expActive, append, onOpen:(path)=>checkedConfig.run("open",path), onApplied:refreshConfig});
  useEffect(()=>{const fps=profiles.activeProfile?.display_fps;if(typeof fps==="number"&&Number.isFinite(fps))setPreviewFpsLimit(Math.min(240,Math.max(1,fps)));},[profiles.activeProfile]);
  return { checkedConfig, profiles };
}
export type ConfigDocumentsModel = ReturnType<typeof useConfigDocuments>;
