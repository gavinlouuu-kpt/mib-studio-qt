// YOFO Review — the standalone review product (plan
// 2026-10-01-standalone-review-app, ADR 0014), rebuilt on the @mib/ui kit
// (#717 B1, #269): AppShell with the Review app's stages Open (R1), Overview
// (R2), Inspect and Export. Mounted by `review.html` / `main.tsx`. MIB Studio's
// Review tab does not use `ReviewPanel` yet (#512 step B5).
//
// This shell owns what the panel does not: backend initialization, the stage
// tabs, preferences (px→µm fallback), updates, the log drawer and the About
// dialog. Inspect is today's `ReviewPanel`, unchanged (frames, charts, its
// toolbar's exports and regenerate); it stays mounted while another stage
// shows, so the open run, the selection and a running job survive a stage
// change. Selecting a stage only changes what is shown. No camera, experiment
// or hardware controls exist in this product.

import { useCallback, useEffect, useRef, useState } from "react";
import { getVersion } from "@tauri-apps/api/app";
import { listen } from "@tauri-apps/api/event";
import "@mib/ui/fonts.css";
import "@mib/ui/style.css";
import { AppShell, Button, StateChip } from "@mib/ui";
import { PanelErrorBoundary } from "../components/PanelErrorBoundary";
import { metricNumber } from "../metricFormat";
import { bridge } from "../bridge";
import { REVIEW_ABI_VERSION } from "./reviewContract";
import { FramePullScheduler } from "../framePullScheduler";
import { ReviewPanel, type OpenOutcome, type ReviewPanelHandle } from "./ReviewPanel";
import { reviewBridge, type ReviewInfo } from "./reviewBridge";
import { checkUpdate, loadUpdateChannel, saveUpdateChannel, UpdateDialog, type UpdateChannel } from "./updates";
import { baseName } from "./exports/exportHelpers";
import { OpenView } from "./open/OpenView";
import { ExportNotice, OverviewNotice } from "./shell/StageNotices";
import "../App.css";
import "./shell/shell.css";

/** The Review app's stages in this build (UI language: Open, Overview, Inspect, …, Export;
 * Populations and Compare are reference views, not scheduled). */
export const REVIEW_STAGES = ["Open", "Overview", "Inspect", "Export"] as const;
export const STAGE = { Open: 0, Overview: 1, Inspect: 2, Export: 3 } as const;

export const PRODUCT_NAME = "YOFO Review";
/** Backend event: a file the OS asked to open (src-tauri review::OPEN_FILE_EVENT). */
export const OPEN_FILE_EVENT = "review-open-file";
// Preferences (per machine; Qt QSettings → localStorage).
export const PX_TO_UM_KEY = "yofo.review.pixelToMicron";
// Same default as the Qt tab's live factor and ReviewSession (0.4886 µm/px).
export const DEFAULT_PX_TO_UM = 0.4886;

/** The stored fallback factor, or the default when unset / invalid. */
export function loadPixelToMicron(storage: Pick<Storage, "getItem"> | null = safeStorage()): number {
  try {
    const v = Number(storage?.getItem(PX_TO_UM_KEY));
    return Number.isFinite(v) && v > 0 ? v : DEFAULT_PX_TO_UM;
  } catch {
    return DEFAULT_PX_TO_UM;
  }
}

function safeStorage(): Storage | null {
  try {
    return typeof localStorage === "undefined" ? null : localStorage;
  } catch {
    return null;
  }
}

function PreferencesDialog(props: {
  value: number;
  channel: UpdateChannel;
  onSave: (v: number, channel: UpdateChannel) => void;
  onClose: () => void;
}) {
  const [text, setText] = useState(String(props.value));
  const [channel, setChannel] = useState<UpdateChannel>(props.channel);
  const v = Number(text);
  const valid = Number.isFinite(v) && v > 0 && v < 1000;
  return (
    <div className="modal-backdrop" onClick={props.onClose}>
      <div className="modal" role="dialog" aria-label="Preferences" onClick={(e) => e.stopPropagation()}>
        <h3>Preferences</h3>
        <div className="row">
          <label htmlFor="pref-px">Fallback px→µm</label>
          <input id="pref-px" type="number" step="0.0001" min="0" value={text} onChange={(e) => setText(e.target.value)} />
        </div>
        <p className="hint">
          Used only for files that do not record their pixel-to-micron factor (older recordings). Files that record one always use it.
        </p>
        <div className="row">
          <label htmlFor="pref-channel">Update channel</label>
          <select id="pref-channel" value={channel} onChange={(e) => setChannel(e.target.value === "beta" ? "beta" : "stable")}>
            <option value="stable">Stable</option>
            <option value="beta">Beta (pre-releases)</option>
          </select>
        </div>
        {!valid && <p className="form-error">Enter a positive number.</p>}
        <div className="actions">
          <button className="btn" onClick={props.onClose}>
            Cancel
          </button>
          <button
            className="btn primary"
            disabled={!valid}
            onClick={() => {
              props.onSave(v, channel);
              props.onClose();
            }}
          >
            Save
          </button>
        </div>
      </div>
    </div>
  );
}

export default function ReviewApp() {
  const [abi, setAbi] = useState<number | null>(null);
  const [appVersion, setAppVersion] = useState("");
  const [ready, setReady] = useState(false);
  const [log, setLog] = useState<string[]>([]);
  const [showLog, setShowLog] = useState(false);
  const [showAbout, setShowAbout] = useState(false);
  const [fitWindow, setFitWindow] = useState(true);
  const [filePath, setFilePath] = useState("");
  const [info, setInfo] = useState<ReviewInfo | null>(null);
  const [pxToUm, setPxToUm] = useState(() => loadPixelToMicron());
  const [showPrefs, setShowPrefs] = useState(false);
  const [channel, setChannel] = useState<UpdateChannel>(() => loadUpdateChannel());
  const [showUpdate, setShowUpdate] = useState(false);
  const [updateNotice, setUpdateNotice] = useState("");
  const [stage, setStage] = useState<number>(STAGE.Open);
  const scheduler = useRef(new FramePullScheduler());
  const panel = useRef<ReviewPanelHandle>(null);
  // React StrictMode runs the boot effect twice in development; open the
  // launch file once.
  const launched = useRef(false);

  const append = useCallback((line: string) => {
    setLog((l) => [`${new Date().toLocaleTimeString()} ${line}`, ...l].slice(0, 50));
    void bridge.shellLog("info", line).catch(() => {});
  }, []);

  // Initialize the backend on boot (empty data dir resolves to Tauri's
  // app_data_dir on the Rust side).
  useEffect(() => {
    getVersion().then(setAppVersion).catch(() => setAppVersion(""));
    reviewBridge
      .abiVersion()
      .then((v) => {
        setAbi(v);
        if (v < REVIEW_ABI_VERSION) append(`review bridge ABI ${v} is older than the UI contract (${REVIEW_ABI_VERSION})`);
      })
      .catch((e) => append(`abi error: ${e}`));
    (async () => {
      try {
        const already = await bridge.isInitialized();
        const ok = already || (await bridge.init(""));
        setReady(ok);
        append(ok ? "backend initialized" : "backend init failed");
        // A file from the command line / file association, or `?open=` (dev
        // and the screenshot harness), opens straight away.
        if (ok && !launched.current) {
          launched.current = true;
          const fromQuery = new URLSearchParams(window.location.search).get("open") ?? "";
          const launch = fromQuery || (await reviewBridge.launchPath().catch(() => ""));
          // A file the operator double-clicked opens straight into Inspect.
          if (launch && (await panel.current?.openPath(launch))?.ok) setStage(STAGE.Inspect);
        }
      } catch (e) {
        append(`init error: ${e}`);
      }
    })();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // macOS: a Finder open (double-click, Open With, drop on the Dock icon)
  // while the app runs. The backend queues the path and emits; taking it
  // through the bridge means a cold-launch open that the boot read already
  // took is never opened twice.
  useEffect(() => {
    if (!ready) return;
    let stop: (() => void) | undefined;
    let disposed = false;
    void listen<string>(OPEN_FILE_EVENT, async () => {
      const path = await reviewBridge.takeOpenRequest().catch(() => "");
      if (path && (await panel.current?.openPath(path))?.ok) setStage(STAGE.Inspect);
    }).then((un) => {
      if (disposed) un();
      else stop = un;
    });
    return () => {
      disposed = true;
      stop?.();
    };
  }, [ready]);

  // One quiet check per launch / channel change: a status-bar notice when a
  // newer version exists (builds without an update key stay silent).
  useEffect(() => {
    let live = true;
    checkUpdate(channel)
      .then((s) => live && setUpdateNotice(s.configured && s.available ? `Update ${s.version} available` : ""))
      .catch(() => live && setUpdateNotice(""));
    return () => {
      live = false;
    };
  }, [channel]);

  useEffect(() => {
    scheduler.current.invalidate();
  }, [filePath]);

  const summary = info?.file_open
    ? `${info.recording_file ? "recording" : "experiment"} · valid ${info.total_valid} · invalid ${info.total_invalid} · px→µm ${metricNumber(info.pixel_to_micron, 4)}${info.pixel_to_micron_from_file ? "" : " (fallback)"}`
    : "no file";

  const openPath = useCallback(
    async (path: string): Promise<OpenOutcome> => (await panel.current?.openPath(path)) ?? { ok: false, message: "The review view is not ready" },
    [],
  );
  const openFile = useCallback(async () => {
    await panel.current?.openFile();
  }, []);
  const closeFile = useCallback(async () => {
    await panel.current?.closeFile();
  }, []);
  const inspect = useCallback(() => setStage(STAGE.Inspect), []);

  return (
    <div className="review-app rv-app">
      <AppShell
        className="rv-shell"
        brand={PRODUCT_NAME}
        tag="Analysis · PC"
        context={[{ label: "Open run", value: info?.file_open ? baseName(info.file_path) : "None" }]}
        stages={[...REVIEW_STAGES]}
        activeStage={stage}
        onStageSelect={setStage}
        status={
          <>
            <StateChip tone={ready ? "confirmed" : "unknown"}>{ready ? "Backend ready" : "Backend · not initialized"}</StateChip>
            <StateChip tone="neutral">Read-only · opening never modifies a file</StateChip>
          </>
        }
        headerActions={
          <div className="rv-header-actions">
            <Button size="small" onClick={() => setFitWindow((f) => !f)} title="Image scaling in Inspect">
              {fitWindow ? "Fit: 1:1" : "Fit: Window"}
            </Button>
            <Button size="small" onClick={() => setShowPrefs(true)}>
              Settings
            </Button>
            <Button size="small" onClick={() => setShowUpdate(true)}>
              Updates…
            </Button>
            <Button size="small" onClick={() => setShowAbout(true)}>
              About
            </Button>
          </div>
        }
      >
        <div className="rv-stage" hidden={stage !== STAGE.Open}>
          <OpenView ready={ready} info={info} onOpenPath={openPath} onOpenFile={openFile} onClose={closeFile} onInspect={inspect} log={append} />
        </div>
        {stage === STAGE.Overview && <OverviewNotice />}
        {/* Inspect: today's review panel, always mounted (see the header). */}
        <div className="review-main rv-inspect" data-testid="review-inspect" hidden={stage !== STAGE.Inspect}>
          <PanelErrorBoundary>
            <ReviewPanel
              ref={panel}
              ready={ready}
              scheduler={scheduler.current}
              fitWindow={fitWindow}
              log={append}
              onFileChange={setFilePath}
              onInfo={setInfo}
              fallbackPixelToMicron={pxToUm}
            />
          </PanelErrorBoundary>
        </div>
        {stage === STAGE.Export && <ExportNotice fileOpen={!!info?.file_open} onInspect={inspect} />}
      </AppShell>

      <div className="statusbar">
        <span className="metrics mono">{filePath || "No file"} · {summary}</span>
        {updateNotice && (
          <button className="log-toggle update-notice" onClick={() => setShowUpdate(true)}>
            {updateNotice}
          </button>
        )}
        <button className="log-toggle" onClick={() => setShowLog((s) => !s)} aria-expanded={showLog}>
          Log ({log.length})
        </button>
      </div>
      {showLog && (
        <div className="log-drawer" role="log">
          {log.map((l, i) => (
            <div key={i}>{l}</div>
          ))}
        </div>
      )}

      {showPrefs && (
        <PreferencesDialog
          value={pxToUm}
          channel={channel}
          onClose={() => setShowPrefs(false)}
          onSave={(v, ch) => {
            setChannel(ch);
            saveUpdateChannel(ch);
            setPxToUm(v);
            try {
              localStorage.setItem(PX_TO_UM_KEY, String(v));
            } catch {
              // Storage unavailable: this session only.
            }
            append(`fallback px→µm set to ${v}`);
          }}
        />
      )}
      {showUpdate && <UpdateDialog channel={channel} onClose={() => setShowUpdate(false)} log={append} />}
      {showAbout && (
        <div className="modal-backdrop" onClick={() => setShowAbout(false)}>
          <div className="modal" onClick={(e) => e.stopPropagation()} role="dialog" aria-label="About">
            <h3>{PRODUCT_NAME}{appVersion ? ` ${appVersion}` : ""}</h3>
            <p>
              Reviews HDF5 files recorded by MIB Studio.
              <br />
              Review bridge ABI version: {abi ?? "unknown"} · backend {ready ? "initialized" : "not initialized"}.
            </p>
            <div className="actions">
              <button className="btn" onClick={() => setShowAbout(false)}>Close</button>
            </div>
          </div>
        </div>
      )}
    </div>
  );
}
