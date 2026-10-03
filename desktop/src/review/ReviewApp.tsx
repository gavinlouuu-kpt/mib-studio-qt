// YOFO Review — the standalone review product (plan
// 2026-10-01-standalone-review-app, ADR 0008). Mounted by `review.html` /
// `main.tsx`; MIB Studio mounts the same `ReviewPanel` in its Review tab.
//
// This shell owns what the panel does not: backend initialization, the
// menu bar, preferences (px→µm fallback), the log drawer and the About
// dialog. The panel drains review events and tracks its jobs itself. No
// camera, experiment or hardware controls exist in this product.

import { useCallback, useEffect, useRef, useState } from "react";
import { getVersion } from "@tauri-apps/api/app";
import { listen } from "@tauri-apps/api/event";
import { bridge } from "../bridge";
import { BRIDGE_ABI_VERSION } from "../bridgeContract";
import { FramePullScheduler } from "../framePullScheduler";
import { ReviewPanel, type ReviewPanelHandle } from "./ReviewPanel";
import { reviewBridge, type ReviewInfo } from "./reviewBridge";
import "../App.css";

export const PRODUCT_NAME = "YOFO Review";
/** Backend event: a file the OS asked to open (src-tauri review::OPEN_FILE_EVENT). */
export const OPEN_FILE_EVENT = "review-open-file";
// Preferences (per machine; Qt QSettings → localStorage).
export const PX_TO_UM_KEY = "yofo.review.pixelToMicron";
export const DEFAULT_PX_TO_UM = 1.0;

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

function PreferencesDialog(props: { value: number; onSave: (v: number) => void; onClose: () => void }) {
  const [text, setText] = useState(String(props.value));
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
        {!valid && <p className="form-error">Enter a positive number.</p>}
        <div className="actions">
          <button className="btn" onClick={props.onClose}>
            Cancel
          </button>
          <button
            className="btn primary"
            disabled={!valid}
            onClick={() => {
              props.onSave(v);
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
        if (v < BRIDGE_ABI_VERSION) append(`bridge ABI ${v} is older than the UI contract (${BRIDGE_ABI_VERSION})`);
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
          if (launch) await panel.current?.openPath(launch);
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
      if (path) await panel.current?.openPath(path);
    }).then((un) => {
      if (disposed) un();
      else stop = un;
    });
    return () => {
      disposed = true;
      stop?.();
    };
  }, [ready]);

  useEffect(() => {
    scheduler.current.invalidate();
  }, [filePath]);

  const summary = info?.file_open
    ? `${info.recording_file ? "recording" : "experiment"} · valid ${info.total_valid} · invalid ${info.total_invalid} · px→µm ${info.pixel_to_micron.toFixed(4)}${info.pixel_to_micron_from_file ? "" : " (fallback)"}`
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
        <Menu label="Help" items={[{ label: "About", onClick: () => setShowAbout(true) }]} />
        <div className="menubar-spacer" />
        <span className="product-name">{PRODUCT_NAME}</span>
      </nav>

      <main className="review-main">
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
      </main>

      <div className="statusbar">
        <span className="metrics mono">{filePath || "No file"} · {summary}</span>
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
          onClose={() => setShowPrefs(false)}
          onSave={(v) => {
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
      {showAbout && (
        <div className="modal-backdrop" onClick={() => setShowAbout(false)}>
          <div className="modal" onClick={(e) => e.stopPropagation()} role="dialog" aria-label="About">
            <h3>{PRODUCT_NAME}{appVersion ? ` ${appVersion}` : ""}</h3>
            <p>
              Reviews HDF5 files recorded by MIB Studio.
              <br />
              Bridge ABI version: {abi ?? "unknown"} · backend {ready ? "initialized" : "not initialized"}.
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
