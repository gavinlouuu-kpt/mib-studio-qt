// YOFO Review updates (plan 2026-10-01-standalone-review-app, PR 6): the
// channel preference, the startup check and Help ▸ Check for updates…. The
// backend (src-tauri/src/review_update.rs) checks
// https://updates.yofo.bio/review-<channel>/latest.json, verifies the bundle's
// minisign signature and pinned SHA-256, installs and restarts. Builds without
// the public key report `configured: false` and the dialog says so.
import { useEffect, useState } from "react";
import { invoke } from "@tauri-apps/api/core";

export type UpdateChannel = "stable" | "beta";
export const UPDATE_CHANNEL_KEY = "yofo.review.updateChannel";

export interface UpdateStatus {
  configured: boolean;
  channel: string;
  current: string;
  available: boolean;
  version: string;
  notes: string;
  date: string;
}

export const checkUpdate = (channel: UpdateChannel) => invoke<UpdateStatus>("review_check_update", { channel });
export const installUpdate = (channel: UpdateChannel) => invoke<void>("review_install_update", { channel });

export function loadUpdateChannel(storage: Pick<Storage, "getItem"> | null = safeStorage()): UpdateChannel {
  try {
    return storage?.getItem(UPDATE_CHANNEL_KEY) === "beta" ? "beta" : "stable";
  } catch {
    return "stable";
  }
}

export function saveUpdateChannel(channel: UpdateChannel): void {
  try {
    safeStorage()?.setItem(UPDATE_CHANNEL_KEY, channel);
  } catch {
    // Storage unavailable: this session only.
  }
}

function safeStorage(): Storage | null {
  try {
    return typeof localStorage === "undefined" ? null : localStorage;
  } catch {
    return null;
  }
}

/** One-line description of a check result. */
export function updateSummary(s: UpdateStatus): string {
  if (!s.configured) return "This build has no update key, so it cannot update itself. Download new versions from the GitHub Release.";
  if (!s.available) return `YOFO Review ${s.current} is the latest version on the ${s.channel} channel.`;
  return `YOFO Review ${s.version} is available (you have ${s.current}).`;
}

type Phase = { kind: "checking" } | { kind: "result"; status: UpdateStatus } | { kind: "installing" } | { kind: "error"; message: string };

export function UpdateDialog(props: { channel: UpdateChannel; onClose: () => void; log: (line: string) => void }) {
  const { channel, onClose, log } = props;
  const [phase, setPhase] = useState<Phase>({ kind: "checking" });

  useEffect(() => {
    let live = true;
    checkUpdate(channel)
      .then((status) => live && setPhase({ kind: "result", status }))
      .catch((e) => live && setPhase({ kind: "error", message: String(e) }));
    return () => {
      live = false;
    };
  }, [channel]);

  const install = async () => {
    setPhase({ kind: "installing" });
    try {
      await installUpdate(channel); // restarts the app on success
    } catch (e) {
      log(`update failed: ${e}`);
      setPhase({ kind: "error", message: String(e) });
    }
  };

  const status = phase.kind === "result" ? phase.status : null;
  return (
    <div className="modal-backdrop" onClick={phase.kind === "installing" ? undefined : onClose}>
      <div className="modal" role="dialog" aria-label="Software update" onClick={(e) => e.stopPropagation()}>
        <h3>Software update</h3>
        {phase.kind === "checking" && <p>Checking the {channel} channel…</p>}
        {phase.kind === "installing" && <p>Downloading and verifying the update; the app restarts when it is installed…</p>}
        {phase.kind === "error" && <p className="form-error">Could not update: {phase.message}</p>}
        {status && (
          <>
            <p>{updateSummary(status)}</p>
            {status.available && status.notes && <pre className="update-notes">{status.notes}</pre>}
          </>
        )}
        <div className="actions">
          <button className="btn" onClick={onClose} disabled={phase.kind === "installing"}>
            Close
          </button>
          {status?.available && (
            <button className="btn primary" onClick={() => void install()}>
              Install and restart
            </button>
          )}
        </div>
      </div>
    </div>
  );
}
