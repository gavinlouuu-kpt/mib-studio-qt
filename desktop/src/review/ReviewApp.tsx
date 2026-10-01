// YOFO Review — the standalone review product (plan
// 2026-10-01-standalone-review-app, ADR 0008). Mounted by `review.html` /
// `main.tsx`; MIB Studio mounts the same `ReviewPanel` in its Review tab.
//
// This shell owns what the panel does not: backend initialization, the
// event drain (PlaybackPosition range, operation outcomes, backend errors),
// the menu bar, the log drawer and the About dialog. No camera, experiment
// or hardware controls exist in this product.

import { useCallback, useEffect, useRef, useState } from "react";
import { getVersion } from "@tauri-apps/api/app";
import { bridge, type BridgeEvent, type ReviewMetadata } from "../bridge";
import { BRIDGE_ABI_VERSION } from "../bridgeContract";
import { FramePullScheduler } from "../framePullScheduler";
import { ReviewPanel, type PlaybackRange, type ReviewPanelHandle } from "./ReviewPanel";
import "../App.css";
import "./review.css";

export const PRODUCT_NAME = "YOFO Review";
const POLL_MS = 200;

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
  const [range, setRange] = useState<PlaybackRange>({ earliest: "0", latest: "0", count: "0" });
  const [filePath, setFilePath] = useState("");
  const [meta, setMeta] = useState<ReviewMetadata | null>(null);
  const scheduler = useRef(new FramePullScheduler());
  const panel = useRef<ReviewPanelHandle>(null);
  const tickBusy = useRef(false);

  const append = useCallback((line: string) => {
    setLog((l) => [`${new Date().toLocaleTimeString()} ${line}`, ...l].slice(0, 50));
    void bridge.shellLog("info", line).catch(() => {});
  }, []);

  const applyEvents = useCallback(
    (events: BridgeEvent[]) => {
      for (const e of events) {
        if (e.kind === "PlaybackPosition") {
          setRange({ earliest: e.earliest, latest: e.latest, count: e.available });
        } else if (e.kind === "BackendError") {
          append(`backend error: ${e.message}${e.textTruncated ? " (details truncated)" : ""}`);
        } else if (e.kind === "OperationStatus") {
          if (e.state === 2) append(`operation ${e.operationId} completed: ${e.message}`);
          else if (e.state >= 3) append(`operation ${e.operationId} ${e.state === 3 ? "failed" : e.state === 4 ? "cancelled" : "timed out"}: ${e.message}`);
        } else if (e.kind === "QueueOverflow") {
          append(`notification loss: ${e.notificationsDropped} discarded (total ${e.notificationsDroppedTotal})`);
        } else if (e.kind === "Unknown") {
          append(`unrecognized notification ${e.receivedKind}`);
        }
      }
    },
    [append],
  );

  // Initialize the backend on boot (empty data dir resolves to Tauri's
  // app_data_dir on the Rust side).
  useEffect(() => {
    getVersion().then(setAppVersion).catch(() => setAppVersion(""));
    bridge
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
      } catch (e) {
        append(`init error: ${e}`);
      }
    })();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // Drain events while a file is open (operation outcomes, playback range).
  useEffect(() => {
    if (!ready) return;
    const id = window.setInterval(async () => {
      if (tickBusy.current) return;
      tickBusy.current = true;
      try {
        applyEvents(await bridge.pollEvents());
      } catch (e) {
        append(`tick error: ${e}`);
      } finally {
        tickBusy.current = false;
      }
    }, POLL_MS);
    return () => window.clearInterval(id);
  }, [ready, applyEvents, append]);

  useEffect(() => {
    scheduler.current.invalidate();
  }, [filePath]);

  const summary = meta?.valid
    ? `${meta.recording_file ? "recording" : "experiment"} · valid ${meta.total_valid} · invalid ${meta.total_invalid}`
    : "no file";

  return (
    <div className="review-app">
      <nav className="menubar" aria-label="Main menu">
        <Menu label="File" items={[{ label: "Open…", onClick: () => void panel.current?.openFile() }]} />
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
          range={range}
          fitWindow={fitWindow}
          log={append}
          applyEvents={applyEvents}
          onFileChange={setFilePath}
          onMetadata={setMeta}
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
