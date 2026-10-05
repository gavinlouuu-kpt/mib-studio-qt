// Central Methods panel (bridge schema v24, issue #398). It only enqueues backend registry commands and
// renders the worker snapshot (polled while open); the UI never waits on the
// network. Read-only toward the instrument: nothing here selects or applies a
// method.
import { useCallback, useEffect, useRef, useState } from "react";
import { bridge, type RegistrySnapshot } from "./bridge";
import { REGISTRY_SESSION_STATES } from "./bridgeContract";
import { CENTRAL_STATE_NOTE, changed, toRegistryView } from "./registry";

const POLL_MS = 250;

export function CentralMethodsPanel(props: { onClose: () => void; onError?: (message: string) => void }) {
  const [snapshot, setSnapshot] = useState<RegistrySnapshot | null>(null);
  const [email, setEmail] = useState("");
  const [password, setPassword] = useState("");
  const [inputError, setInputError] = useState("");
  const last = useRef<RegistrySnapshot | null>(null);
  const report = props.onError;

  const poll = useCallback(async () => {
    try {
      const next = await bridge.fetchRegistrySnapshot();
      if (changed(last.current, next)) {
        last.current = next;
        setSnapshot(next);
      }
    } catch (e) {
      report?.(`registry snapshot failed: ${e}`);
    }
  }, [report]);

  // Opening the panel is an explicit sync point (#398): refresh once when
  // signed in; otherwise just poll the snapshot while the panel is open.
  useEffect(() => {
    let alive = true;
    void (async () => {
      try {
        const first = await bridge.fetchRegistrySnapshot();
        if (!alive) return;
        last.current = first;
        setSnapshot(first);
        if (first.session === REGISTRY_SESSION_STATES.SignedIn) await bridge.registryRefresh();
      } catch (e) {
        report?.(`registry snapshot failed: ${e}`);
      }
    })();
    const id = window.setInterval(() => void poll(), POLL_MS);
    return () => {
      alive = false;
      window.clearInterval(id);
    };
  }, [poll, report]);

  const view = snapshot ? toRegistryView(snapshot) : null;

  useEffect(() => {
    if (view?.email && !email) setEmail(view.email);
  }, [view?.email, email]);

  const run = (action: () => Promise<unknown>) => {
    void action()
      .then(() => poll())
      .catch((e) => report?.(`registry command failed: ${e}`));
  };

  const signIn = () => {
    const pw = password;
    setPassword(""); // the backend worker holds the only remaining copy
    if (!email.trim() || !pw) {
      setInputError("Enter an email and a password.");
      return;
    }
    setInputError("");
    run(() => bridge.registrySignIn(email.trim(), pw));
  };

  const warnings = [...(inputError ? [inputError] : []), ...(view?.warnings ?? [])];

  return (
    <div className="modal-backdrop" onClick={props.onClose}>
      <div
        className="modal registry-modal"
        onClick={(e) => e.stopPropagation()}
        role="dialog"
        aria-label="Central Methods"
      >
        <h3>Central Methods</h3>
        <p className="registry-status" data-testid="registry-status">
          {view?.status ?? "Central registry: loading..."}
        </p>
        <p data-testid="registry-account">{view?.account ?? ""}</p>
        {view && view.activity.length > 0 && <p className="registry-activity">{view.activity.join(" | ")}</p>}
        {warnings.length > 0 && (
          <div className="registry-warning" role="alert">
            {warnings.map((w) => (
              <div key={w}>{w}</div>
            ))}
          </div>
        )}

        {view?.showSignIn && (
          <form
            className="row"
            onSubmit={(e) => {
              e.preventDefault();
              signIn();
            }}
          >
            <input
              type="text"
              placeholder="Email"
              autoComplete="username"
              value={email}
              onChange={(e) => setEmail(e.target.value)}
            />
            <input
              type="password"
              placeholder="Password"
              autoComplete="current-password"
              value={password}
              onChange={(e) => setPassword(e.target.value)}
            />
            <button className="btn" type="submit" disabled={!view.canSignIn}>
              Sign in
            </button>
          </form>
        )}

        <div className="row">
          <button className="btn" disabled={!view?.canRefresh} onClick={() => run(() => bridge.registryRefresh())}>
            Refresh
          </button>
          <button className="btn" disabled={!view?.canCancel} onClick={() => run(() => bridge.registryCancelAll())}>
            Cancel
          </button>
          <span style={{ flex: 1 }} />
          <button className="btn" disabled={!view?.canSignOut} onClick={() => run(() => bridge.registrySignOut())}>
            Sign out
          </button>
        </div>

        <div className="registry-table-wrap">
          <table className="registry-table">
            <thead>
              <tr>
                <th>Method</th>
                <th>Revision</th>
                <th>Project</th>
                <th>Central state</th>
                <th>Content hash</th>
                <th>Author</th>
              </tr>
            </thead>
            <tbody>
              {(view?.rows ?? []).map((r) => (
                <tr key={r.revisionId} className={r.revoked ? "revoked" : undefined}>
                  <td>{r.method}</td>
                  <td>{r.revision}</td>
                  <td>{r.project}</td>
                  <td>{r.state}</td>
                  <td title={r.hash}>
                    <code>{r.hashPrefix}</code>
                  </td>
                  <td>{r.author}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
        <p className="registry-note">{CENTRAL_STATE_NOTE}</p>
        <div className="actions">
          <button className="btn" onClick={props.onClose}>
            Close
          </button>
        </div>
      </div>
    </div>
  );
}
