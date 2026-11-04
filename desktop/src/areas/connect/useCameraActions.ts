// Connect and Overview (epic #863, B2): what surrounds Start / Stop Camera: the capture lifecycle behind the phase text, the
// space-bar shortcut, the profile's expected camera and the configured autofocus staleness. Hooks only; the rules are in
// connectFlow.ts (Qt: src/frontend/controllers/CameraController.cpp, MainWindow.cpp).
import { useEffect, useRef } from "react";
import { bridge } from "../../bridge";
import { configDocument } from "../../configDocument";
import { expectedCameraModel } from "../../preflight";
import type { ConfigDocumentsModel } from "../config/useConfig";
import type { HardwareState } from "../hardware/useHardware";
import type { MonitoringState } from "../monitoring/useMonitoring";
import type { PreviewState } from "../preview/usePreview";
import type { ShellState } from "../shell/useShell";
import { spaceCameraAction, spaceTogglesCamera } from "./connectFlow";
import type { ConnectState, ConnectModel, CameraConnectionModel } from "./useConnect";

type PhaseDeps = Pick<ShellState, "ready"> & Pick<ConnectState, "setLifecycle" | "cameraCommandRef">;
const POLL_MS = 250;   // Qt's controller polls the lifecycle every 250 ms
const SETTLE_MS = 60;  // and every 20 ms while a command settles; the bridge is asked a little less often

// The capture lifecycle (Starting / Running / Stopping / Faulted and the last failure) for the phase text beside Start / Stop.
export function useCameraPhase({ ready, setLifecycle, cameraCommandRef }: PhaseDeps) {
  useEffect(() => {
    if (!ready) { setLifecycle(null); return; }
    let alive = true, timer = 0;
    const poll = async () => {
      let next = POLL_MS;
      try {
        const value = await bridge.fetchCaptureLifecycle();
        if (!alive) return;
        setLifecycle(value.valid ? value : null);
        if (value.valid && (value.state === "starting" || value.state === "stopping")) next = SETTLE_MS;
      } catch {
        /* the shell's own poll reports a lost backend; the phase falls back to the running flag */
      }
      if (alive) timer = window.setTimeout(() => void poll(), cameraCommandRef.current ? SETTLE_MS : next);
    };
    void poll();
    return () => { alive = false; window.clearTimeout(timer); };
  }, [ready]);
}

type ShortcutDeps = Pick<ShellState, "ready" | "tab"> & Pick<MonitoringState, "expActive" | "expTab"> & Pick<PreviewState, "recording">
  & Pick<ConnectState, "running" | "cameraCommand"> & Pick<ConnectModel, "startCameraReason">
  & Pick<CameraConnectionModel, "onStartCamera" | "onStopCamera" | "refuse">;

// CONNECT-041: the space bar starts or stops the camera through the same guarded path as the buttons.
export function useCameraShortcut(deps: ShortcutDeps) {
  const live = useRef(deps);
  live.current = deps;
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      const d = live.current;
      const el = e.target instanceof Element ? e.target : null;
      const target = el ? {
        tagName: el.tagName,
        isContentEditable: (el as HTMLElement).isContentEditable === true,
        role: el.getAttribute("role"),
        inDialog: !!el.closest('[role="dialog"], [role="alertdialog"], .modal-backdrop'),
      } : null;
      if (!spaceTogglesCamera({ key: e.key, code: e.code, repeat: e.repeat, ctrlKey: e.ctrlKey, altKey: e.altKey, metaKey: e.metaKey,
        shiftKey: e.shiftKey, defaultPrevented: e.defaultPrevented, target }, { tab: d.tab, expTab: d.expTab, expActive: d.expActive, ready: d.ready })) return;
      if (document.querySelector(".modal-backdrop")) return; // a dialog is open: the keys are its
      e.preventDefault();
      const action = spaceCameraAction({ running: d.running, recording: d.recording, commandInFlight: d.cameraCommand !== null, startReason: d.startCameraReason });
      if (action.kind === "stop") void d.onStopCamera();
      else if (action.kind === "start") void d.onStartCamera();
      else if (action.kind === "refuse") d.refuse("Camera", action.message);
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, []);
}

type ProfileDeps = Pick<ShellState, "ready" | "tab"> & Pick<ConnectState, "setExpectedCamera" | "setFocusStaleMs">
  & Pick<ConfigDocumentsModel, "profiles"> & Pick<HardwareState, "afStatus"> & Pick<ShellState, "caps">;

// App.tsx:1237 / App.tsx:1306: the camera the active profile was made for (camera.window.camera_model, #833) and the staleness
// of the focus metric the operator configured (ring_ratio_stale_ms), both read where the profile and the autofocus config are.
export function useProfileCameraContext({ ready, tab, caps, profiles, afStatus, setExpectedCamera, setFocusStaleMs }: ProfileDeps) {
  const path = profiles.activeProfile?.path ?? "", revision = profiles.activeProfile?.revision ?? "";
  useEffect(() => {
    if (!ready || !path) { setExpectedCamera(""); return; }
    let alive = true;
    configDocument.read(path)
      .then((doc) => { if (alive) setExpectedCamera(doc.ok ? expectedCameraModel(doc.document_json) : ""); })
      .catch(() => { if (alive) setExpectedCamera(""); });
    return () => { alive = false; };
  }, [ready, path, revision]);

  const connected = afStatus?.connected ?? false;
  useEffect(() => {
    if (!ready || !caps.autofocus || (tab !== "overview" && tab !== "connect")) return;
    let alive = true;
    bridge.fetchAutofocusConfig()
      .then((config) => { if (alive && config.valid && config.ring_ratio_stale_ms > 0) setFocusStaleMs(config.ring_ratio_stale_ms); })
      .catch(() => { /* keep the last known staleness */ });
    return () => { alive = false; };
  }, [ready, caps.autofocus, tab, revision, connected]);
}
