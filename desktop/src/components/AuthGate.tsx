import { Fragment, useCallback, useEffect, useRef, useState, type ReactNode } from "react";
import { isRemote } from "../transport";
import { authUrl, probeAuthDetail, storeToken, type AuthOutcome } from "../transport/auth";
import "./AuthGate.css";

// #501: in a browser against the instrument, the app mounts only after the server accepts the
// token; otherwise the operator is asked for it. The desktop shell (Tauri) passes straight through.
//
// Once connected the gate keeps watching `/auth` (every `monitorMs`): a lost link shows a banner and
// is retried; when the server answers again with the same boot id the page just carries on; with a
// different boot id (the backend restarted or the board rebooted) the app is re-mounted so it loads
// the new backend's state, without a page reload; a rejected token (a new token after a restart)
// brings the prompt back over the running app.
const MONITOR_MS = 2000;
const LOST_AFTER_FAILURES = 2;

function TokenPrompt({ rejected, onSubmit, overlay }: { rejected: boolean; onSubmit: (token: string) => void; overlay?: boolean }) {
  const [token, setToken] = useState("");
  return (
    <form
      className={overlay ? "auth-gate auth-gate-overlay" : "auth-gate"}
      aria-label="Instrument access token"
      onSubmit={(e) => {
        e.preventDefault();
        onSubmit(token.trim());
      }}
    >
      <h2>Access token</h2>
      <p>This instrument requires its access token (on the instrument: /etc/yofo-studio/token).</p>
      {rejected && <p role="alert">The token was not accepted.</p>}
      <label>
        Token
        <input type="password" autoComplete="current-password" value={token} onChange={(e) => setToken(e.target.value)} autoFocus />
      </label>
      <button type="submit" disabled={!token.trim()}>Connect</button>
    </form>
  );
}

export function AuthGate({ children, monitorMs = MONITOR_MS }: { children: ReactNode; monitorMs?: number }) {
  const [outcome, setOutcome] = useState<AuthOutcome | "checking">(isRemote ? "checking" : "authorized");
  const [rejected, setRejected] = useState(false);
  const [session, setSession] = useState(0);
  const [lost, setLost] = useState(false);
  const [restarted, setRestarted] = useState(false);
  const [reauth, setReauth] = useState(false);
  const bootId = useRef<string | undefined>(undefined);
  const failures = useRef(0);

  const check = useCallback(async () => {
    setOutcome("checking");
    const probe = await probeAuthDetail(authUrl());
    if (probe.bootId) bootId.current = probe.bootId;
    setOutcome(probe.outcome);
  }, []);

  useEffect(() => {
    if (isRemote) void check();
  }, [check]);

  // One probe of the running link: updates the banner, the app session and the prompt.
  const watch = useCallback(async () => {
    const probe = await probeAuthDetail(authUrl());
    if (probe.outcome === "unreachable") {
      failures.current += 1;
      if (failures.current >= LOST_AFTER_FAILURES) setLost(true);
      return;
    }
    failures.current = 0;
    setLost(false);
    setReauth(probe.outcome === "unauthorized");
    if (probe.bootId) {
      if (bootId.current && probe.bootId !== bootId.current) {
        setSession((n) => n + 1);
        setRestarted(true);
      }
      bootId.current = probe.bootId;
    }
    if (probe.outcome === "authorized") setRejected(false);
  }, []);

  useEffect(() => {
    if (!isRemote || outcome !== "authorized") return;
    const id = window.setInterval(() => void watch(), monitorMs);
    return () => window.clearInterval(id);
  }, [outcome, watch, monitorMs]);

  if (outcome === "authorized") {
    return (
      <>
        {lost && <p className="connection-banner" role="status">Connection to the instrument lost. Reconnecting…</p>}
        {restarted && (
          <p className="connection-banner" role="status">
            The instrument restarted: its state was reloaded.{" "}
            <button onClick={() => setRestarted(false)}>Dismiss</button>
          </p>
        )}
        {reauth && (
          <TokenPrompt
            overlay
            rejected={rejected}
            onSubmit={(token) => {
              storeToken(token);
              setRejected(true);
              void watch();
            }}
          />
        )}
        <Fragment key={session}>{children}</Fragment>
      </>
    );
  }
  if (outcome === "checking") return <p className="auth-gate" role="status">Connecting to the instrument…</p>;
  if (outcome === "unreachable") {
    return (
      <div className="auth-gate" role="alert">
        <h2>Instrument unreachable</h2>
        <p>The YOFO Studio server did not answer. Check the network and that yofo-studio is running.</p>
        <button onClick={() => void check()}>Retry</button>
      </div>
    );
  }
  return (
    <TokenPrompt
      rejected={rejected}
      onSubmit={(token) => {
        storeToken(token);
        setRejected(true);
        void check();
      }}
    />
  );
}
