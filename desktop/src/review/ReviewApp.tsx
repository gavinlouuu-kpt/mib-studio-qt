import { PanelErrorBoundary } from "../components/PanelErrorBoundary";
import { metricNumber } from "../metricFormat";
// YOFO Review — the standalone review product (plan
// 2026-10-01-standalone-review-app, ADR 0014). Mounted by `review.html` /
// `main.tsx`. MIB Studio's Review tab does not use `ReviewPanel` yet (#512
// step B5).
//
// This shell owns what the panel does not: backend initialization, the
// menu bar, preferences (px→µm fallback), the log drawer and the About
// dialog. The panel drains review events and tracks its jobs itself. No
// camera, experiment or hardware controls exist in this product.

import { useCallback, useEffect, useRef, useState } from "react";
import { getVersion } from "@tauri-apps/api/app";
import { listen } from "@tauri-apps/api/event";
import { bridge } from "../bridge";
import { isRemote, transport } from "../transport";
import { REVIEW_ABI_VERSION } from "./reviewContract";
import {
  REVIEW_UNAVAILABLE_TEXT,
  isReviewUnavailable,
  reviewAvailability,
  useReviewAvailability,
  watchReviewConnection,
} from "./reviewAvailability";
import { FramePullScheduler } from "../framePullScheduler";
import { ReviewPanel, type ReviewPanelHandle } from "./ReviewPanel";
import { reviewBridge, type ReviewInfo } from "./reviewBridge";
import { checkUpdate, loadUpdateChannel, saveUpdateChannel, UpdateDialog, type UpdateChannel } from "./updates";
import "../App.css";

export const PRODUCT_NAME = "YOFO Review";
/** Backend event: a file the OS asked to open (src-tauri review::OPEN_FILE_EVENT). */
export const OPEN_FILE_EVENT = "review-open-file";
// Preferences (per machine; Qt QSettings → localStorage).
export const PX_TO_UM_KEY = "yofo.review.pixelToMicron";
// Same default as the Qt tab's live factor and ReviewSession (0.4886 µm/px).
export const DEFAULT_PX_TO_UM = 0.4886;
/** A recheck that could not reach the server tries again after this, doubling
 * up to RECHECK_MAX_MS (#763 F7). */
export const RECHECK_FIRST_MS = 1000;
export const RECHECK_MAX_MS = 30000;

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

function Menu(props: { label: string; items: { label: string; onClick?: () => void; pending?: string }[] }) {
  const [openMenu, setOpenMenu] = useState(false);
  return (
    <div className="menubar-item">
      <button aria-expanded={openMenu} onClick={() => setOpenMenu((o) => !o)} onBlur={() => window.setTimeout(() => setOpenMenu(false), 150)}>
        {props.label}
      </button>
      {openMenu && (
        <div className="menu-popup" role="menu">
          {props.items.map((it) => (
            <button
              key={it.label}
              role="menuitem"
              disabled={!!it.pending}
              title={it.pending}
              onClick={() => {
                setOpenMenu(false);
                it.onClick?.();
              }}
            >
              {it.label}
            </button>
          ))}
        </div>
      )}
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
  // #763 F7: a server without the review commands (yofo-studio-server before
  // #512 S11) is "initialized" but cannot review; the panel stays disabled
  // and the shell says why instead of offering an Open that cannot work.
  // A new connection or Retry checks again ("checking") before enabling it.
  const availability = useReviewAvailability();
  const rechecking = availability.state === "checking";
  const reviewUnavailable = availability.state === "unavailable" || rechecking;
  const reviewReady = ready && !reviewUnavailable;
  const scheduler = useRef(new FramePullScheduler());
  const panel = useRef<ReviewPanelHandle>(null);
  // React StrictMode runs the boot effect twice in development; open the
  // launch file once.
  const launched = useRef(false);

  const append = useCallback((line: string) => {
    setLog((l) => [`${new Date().toLocaleTimeString()} ${line}`, ...l].slice(0, 50));
    void bridge.shellLog("info", line).catch(() => {});
  }, []);

  // The review ABI probe, also the availability probe: over the WebSocket an
  // UNKNOWN_COMMAND answer marks the review commands unavailable, a reply
  // available (#763 F7). Resolves "available", "unavailable" or the error.
  const probeReview = useCallback(async (): Promise<string> => {
    try {
      const v = await reviewBridge.abiVersion();
      setAbi(v);
      if (v < REVIEW_ABI_VERSION) append(`review bridge ABI ${v} is older than the UI contract (${REVIEW_ABI_VERSION})`);
      return "available";
    } catch (e) {
      if (!isReviewUnavailable(e)) return String(e);
      append("review commands are not served by this server");
      return "unavailable";
    }
  }, [append]);

  // Initialize the backend on boot (empty data dir resolves to Tauri's
  // app_data_dir on the Rust side).
  useEffect(() => {
    getVersion().then(setAppVersion).catch(() => setAppVersion(""));
    const abiProbe = probeReview().then((r) => {
      if (r !== "available" && r !== "unavailable") append(`abi error: ${r}`);
    });
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
          await abiProbe;
          if (reviewAvailability.get().state === "unavailable") return;
          const fromQuery = new URLSearchParams(window.location.search).get("open") ?? "";
          const launch = fromQuery || (await reviewBridge.launchPath().catch(() => ""));
          if (launch) await panel.current?.openPath(launch);
        }
      } catch (e) {
        append(`init error: ${e}`);
      }
    })();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // The server may be restarted or upgraded while the page is open: a lost or
  // new connection checks the review commands again (none in the Tauri shells).
  useEffect(() => watchReviewConnection(transport.session), []);

  // A recheck (new connection, Retry): probe until the server answers. Only a
  // reply enables the panel and the job poll again; UNKNOWN_COMMAND keeps them
  // off; a server that cannot be reached yet is asked again with backoff.
  useEffect(() => {
    if (!rechecking) return;
    let live = true;
    let timer: number | undefined;
    let delay = RECHECK_FIRST_MS;
    let reported = "";
    const attempt = async () => {
      const outcome = await probeReview();
      // The answer itself ends the recheck (the state left "checking").
      if (outcome === "available") append("review commands are served by this server; review enabled");
      if (!live || outcome === "available" || outcome === "unavailable") return;
      if (outcome !== reported) append(`review check: ${outcome}; trying again`);
      reported = outcome;
      timer = window.setTimeout(() => void attempt(), delay);
      delay = Math.min(delay * 2, RECHECK_MAX_MS);
    };
    void attempt();
    return () => {
      live = false;
      window.clearTimeout(timer);
    };
  }, [rechecking, probeReview, append]);

  // macOS: a Finder open (double-click, Open With, drop on the Dock icon)
  // while the app runs. The backend queues the path and emits; taking it
  // through the bridge means a cold-launch open that the boot read already
  // took is never opened twice.
  useEffect(() => {
    // The browser has no OS open requests (and no Tauri event API).
    if (!reviewReady || isRemote) return;
    let stop: (() => void) | undefined;
    let disposed = false;
    void listen<string>(OPEN_FILE_EVENT, async () => {
      const path = await reviewBridge.takeOpenRequest().catch(() => "");
      if (path) await panel.current?.openPath(path);
    }).then((un) => {
      if (disposed) un();
      else stop = un;
    });
    return () => {
      disposed = true;
      stop?.();
    };
  }, [reviewReady]);

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

  return (
    <div className="review-app">
      <nav className="menubar" aria-label="Main menu">
        <Menu
          label="File"
          items={[
            { label: "Open…", onClick: () => void panel.current?.openFile() },
            { label: "Close", onClick: () => void panel.current?.closeFile() },
            { label: "Preferences…", onClick: () => setShowPrefs(true) },
          ]}
        />
        <Menu label="View" items={[{ label: fitWindow ? "Fit: 1:1" : "Fit: Window", onClick: () => setFitWindow((f) => !f) }]} />
        <Menu
          label="Help"
          items={[
            { label: "Check for updates…", onClick: () => setShowUpdate(true) },
            { label: "About", onClick: () => setShowAbout(true) },
          ]}
        />
        <div className="menubar-spacer" />
        <span className="product-name">{PRODUCT_NAME}</span>
      </nav>

      <main className="review-main">
        {reviewUnavailable && (
          <p className="pending-note" role="alert" data-testid="review-unavailable">
            <span data-testid="review-unavailable-text">{REVIEW_UNAVAILABLE_TEXT}</span>{" "}
            <button className="btn" onClick={() => reviewAvailability.recheck()} disabled={rechecking}>
              {rechecking ? "Checking…" : "Retry"}
            </button>
          </p>
        )}
        <PanelErrorBoundary>
        <ReviewPanel
          ref={panel}
          ready={reviewReady}
          scheduler={scheduler.current}
          fitWindow={fitWindow}
          log={append}
          onFileChange={setFilePath}
          onInfo={setInfo}
          fallbackPixelToMicron={pxToUm}
        />
        </PanelErrorBoundary>
      </main>

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
              Review bridge ABI version: {abi ?? "unknown"} · backend {ready ? "initialized" : "not initialized"}
              {reviewUnavailable ? " · review commands not served by this server" : ""}.
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
