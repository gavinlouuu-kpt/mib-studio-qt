import { useCallback, useEffect, useState, type ReactNode } from "react";
import { isRemote } from "../transport";
import { authUrl, probeAuth, storeToken, type AuthOutcome } from "../transport/auth";
import "./AuthGate.css";

// #501: in a browser against the instrument, the app mounts only after the server accepts the
// token; otherwise the operator is asked for it. The desktop shell (Tauri) passes straight through.
export function AuthGate({ children }: { children: ReactNode }) {
  const [outcome, setOutcome] = useState<AuthOutcome | "checking">(isRemote ? "checking" : "authorized");
  const [token, setToken] = useState("");
  const [rejected, setRejected] = useState(false);

  const check = useCallback(async () => {
    setOutcome("checking");
    setOutcome(await probeAuth(authUrl()));
  }, []);

  useEffect(() => {
    if (isRemote) void check();
  }, [check]);

  if (outcome === "authorized") return <>{children}</>;
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
    <form
      className="auth-gate"
      aria-label="Instrument access token"
      onSubmit={(e) => {
        e.preventDefault();
        storeToken(token.trim());
        setRejected(true);
        void check();
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
