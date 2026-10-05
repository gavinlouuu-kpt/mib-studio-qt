// Central Methods panel (bridge schema v15, issue #398): the React twin of the
// Qt CentralMethodsDialog. It only enqueues backend registry commands and
// renders the worker snapshot (polled while open); the UI never waits on the
// network. #398 M2b: select a row to materialize it or record a local
// validation backed by a test-run file (the backend checks the file was
// recorded with that revision on this instrument). #398 M2c: Apply previews
// the changed config.json keys, asks for confirmation, then loads the
// revision exactly through the backend applier. #398 M3b: review actions by
// role with a required reason, revision details/history, and a Drafts view
// (new method from the current config, release notes, submit, and the
// explicit conflict choices) — the same rules as the Qt dialog (registry.ts).
import { open } from "@tauri-apps/plugin-dialog";
import { useCallback, useEffect, useRef, useState } from "react";
import { bridge, type MethodApplyPlan, type RegistryCommand, type RegistrySnapshot } from "./bridge";
import { REGISTRY_CENTRAL_STATES, REGISTRY_SESSION_STATES } from "./bridgeContract";
import {
  CENTRAL_STATE_NOTE,
  actionsFor,
  applyConfirmText,
  applyResultText,
  authorProjects,
  changed,
  conflictText,
  detailLines,
  draftActionsFor,
  draftRows,
  reviewActionsFor,
  toRegistryView,
} from "./registry";

const H5_FILTER = [{ name: "HDF5 run", extensions: ["h5", "hdf5"] }];

const POLL_MS = 250;

export function CentralMethodsPanel(props: { onClose: () => void; onError?: (message: string) => void }) {
  const [snapshot, setSnapshot] = useState<RegistrySnapshot | null>(null);
  const [email, setEmail] = useState("");
  const [password, setPassword] = useState("");
  const [inputError, setInputError] = useState("");
  const [selected, setSelected] = useState<string | null>(null);
  const [actionError, setActionError] = useState("");
  const [tab, setTab] = useState<"methods" | "drafts">("methods");
  const [reason, setReason] = useState("");
  const [useCurrentConfig, setUseCurrentConfig] = useState(false);
  const [selectedDraft, setSelectedDraft] = useState<string | null>(null);
  const [draftNotes, setDraftNotes] = useState("");
  const [newProject, setNewProject] = useState("");
  const [newName, setNewName] = useState("");
  const [keepDraftConfig, setKeepDraftConfig] = useState(true);
  const [pendingApply, setPendingApply] = useState<MethodApplyPlan | null>(null);
  const [applyNotice, setApplyNotice] = useState("");
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

  const actions = view ? actionsFor(view, snapshot, selected) : null;
  const review = reviewActionsFor(snapshot, selected);
  const draftActions = draftActionsFor(snapshot, selectedDraft);
  const drafts = snapshot ? draftRows(snapshot) : [];
  const projects = snapshot ? authorProjects(snapshot) : [];

  // Authoring commands answer at once with a job (or a refusal and why).
  const command = (action: () => Promise<RegistryCommand>, after?: () => void) => {
    setActionError("");
    void action()
      .then((res) => {
        if (res.job_id === "0") setActionError(res.error || "The request was refused.");
        else after?.();
        return poll();
      })
      .catch((e) => report?.(`registry command failed: ${e}`));
  };

  const previewApply = () => {
    if (!selected) return;
    setActionError("");
    setApplyNotice("");
    void bridge
      .registryPlanApply(selected)
      .then((plan) => (plan.ok ? setPendingApply(plan) : setActionError(applyConfirmText(plan))))
      .catch((e) => report?.(`apply preview failed: ${e}`));
  };

  const confirmApply = () => {
    if (!pendingApply) return;
    const id = pendingApply.revision_id;
    setPendingApply(null);
    void bridge
      .registryApplyMethod(id)
      .then((result) => {
        if (result.ok) setApplyNotice(applyResultText(result));
        else setActionError(applyResultText(result));
        return poll();
      })
      .catch((e) => report?.(`apply failed: ${e}`));
  };

  const transition = (target: number) => {
    if (!selected) return;
    if (!reason.trim()) {
      setActionError("A reason is required; nothing was changed.");
      return;
    }
    command(() => bridge.registryTransition(selected, target, reason), () => setReason(""));
  };

  const markValidated = async (passed: boolean) => {
    if (!selected) return;
    setActionError("");
    try {
      const picked = await open({
        title: passed ? "Test run that validates this revision" : "Test run that failed with this revision",
        filters: H5_FILTER,
        multiple: false,
        directory: false,
      });
      if (typeof picked !== "string") return; // cancelled
      const res = await bridge.registryRecordValidation(selected, picked, passed);
      if (res.job_id === "0") setActionError(res.error || "Validation was refused.");
      await poll();
    } catch (e) {
      report?.(`registry validation failed: ${e}`);
    }
  };

  const warnings = [
    ...(inputError ? [inputError] : []),
    ...(actionError ? [actionError] : []),
    ...(view?.warnings ?? []),
  ];

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
        {view?.instrument && <p data-testid="registry-instrument">{view.instrument}</p>}
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

        <div className="row registry-tabs" role="tablist">
          <button className={`btn${tab === "methods" ? " active" : ""}`} role="tab" aria-selected={tab === "methods"} onClick={() => setTab("methods")}>
            Methods
          </button>
          <button className={`btn${tab === "drafts" ? " active" : ""}`} role="tab" aria-selected={tab === "drafts"} onClick={() => setTab("drafts")}>
            Drafts ({drafts.length})
          </button>
        </div>
        {tab === "methods" && (
          <>
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
                <th>Local validation</th>
              </tr>
            </thead>
            <tbody>
              {(view?.rows ?? []).map((r) => (
                <tr
                  key={r.revisionId}
                  className={[r.revoked ? "revoked" : "", r.revisionId === selected ? "selected" : ""].join(" ").trim() || undefined}
                  onClick={() => setSelected(r.revisionId)}
                  aria-selected={r.revisionId === selected}
                >
                  <td title={r.notes || undefined}>{r.method}</td>
                  <td>{r.revision}</td>
                  <td>{r.project}</td>
                  <td>{r.state}</td>
                  <td title={r.hash}>
                    <code>{r.hashPrefix}</code>
                  </td>
                  <td>{r.author}</td>
                  <td className={r.validationFailed ? "validation-failed" : undefined}>
                    {r.validation}
                    {r.materialized ? " · files ready" : ""}
                    {r.update ? ` · ${r.update}` : ""}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
        <div className="row" data-testid="registry-actions">
          <button
            className="btn"
            disabled={!actions?.canMaterialize}
            onClick={() => selected && run(() => bridge.registryMaterialize(selected))}
          >
            Materialize
          </button>
          <button className="btn" disabled={!actions?.canApply} title={actions?.applyReason} onClick={previewApply}>
            Apply...
          </button>
          <button className="btn" disabled={!actions?.canMarkValidated} onClick={() => void markValidated(true)}>
            Mark validated...
          </button>
          <button className="btn" disabled={!actions?.canMarkValidated} onClick={() => void markValidated(false)}>
            Record failed run...
          </button>
        </div>
            {pendingApply && (
              <div className="registry-confirm" role="alertdialog" aria-label="Apply central method">
                <pre>{applyConfirmText(pendingApply)}</pre>
                <div className="row">
                  <button className="btn" onClick={confirmApply}>
                    Apply
                  </button>
                  <button className="btn" onClick={() => setPendingApply(null)}>
                    Cancel
                  </button>
                </div>
              </div>
            )}
            {applyNotice && (
              <p className="registry-activity" data-testid="registry-apply-notice">
                {applyNotice}
              </p>
            )}
            <div className="row" data-testid="registry-review">
              <button
                className="btn"
                disabled={!review.canNewDraft}
                onClick={() =>
                  selected &&
                  command(() => bridge.registryNewDraftFromRevision(selected, useCurrentConfig), () => setTab("drafts"))
                }
              >
                New draft
              </button>
              <label className="registry-check">
                <input type="checkbox" checked={useCurrentConfig} onChange={(e) => setUseCurrentConfig(e.target.checked)} />
                from current config.json
              </label>
              <button className="btn" disabled={!review.canApprove} onClick={() => transition(REGISTRY_CENTRAL_STATES.Approved)}>
                Approve
              </button>
              <button className="btn" disabled={!review.canReject} onClick={() => transition(REGISTRY_CENTRAL_STATES.Rejected)}>
                Reject
              </button>
              <button className="btn" disabled={!review.canPublish} onClick={() => transition(REGISTRY_CENTRAL_STATES.Published)}>
                Publish
              </button>
              <button className="btn" disabled={!review.canArchive} onClick={() => transition(REGISTRY_CENTRAL_STATES.Archived)}>
                Archive
              </button>
              <button
                className="btn"
                disabled={!review.canRevoke}
                title="Revocation is permanent: every instrument blocks Start with it."
                onClick={() => transition(REGISTRY_CENTRAL_STATES.Revoked)}
              >
                Revoke
              </button>
              <button
                className="btn"
                disabled={!review.canHistory}
                onClick={() => selected && command(() => bridge.registryFetchHistory(selected))}
              >
                History
              </button>
            </div>
            <textarea
              className="registry-reason"
              placeholder="Reason for approve / reject / publish / archive / revoke (recorded in the audit trail)"
              value={reason}
              onChange={(e) => setReason(e.target.value)}
              rows={2}
            />
            {snapshot && selected && (
              <pre className="registry-details" data-testid="registry-details">
                {detailLines(snapshot, selected).join("\n")}
              </pre>
            )}
          </>
        )}
        {tab === "drafts" && (
          <>
            <div className="registry-table-wrap">
              <table className="registry-table" data-testid="registry-drafts">
                <thead>
                  <tr>
                    <th>Method</th>
                    <th>Based on</th>
                    <th>Status</th>
                    <th>Release notes</th>
                  </tr>
                </thead>
                <tbody>
                  {drafts.map((d) => (
                    <tr
                      key={d.draftId}
                      className={[d.conflict ? "conflict" : "", d.draftId === selectedDraft ? "selected" : ""].join(" ").trim() || undefined}
                      onClick={() => {
                        setSelectedDraft(d.draftId);
                        setDraftNotes(d.notes);
                      }}
                      aria-selected={d.draftId === selectedDraft}
                    >
                      <td>{d.method}</td>
                      <td>{d.base}</td>
                      <td>{d.status}</td>
                      <td title={d.notes}>{d.notes.split("\n")[0]}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
            {snapshot && conflictText(snapshot, selectedDraft) && (
              <pre className="registry-conflict" role="alert">
                {conflictText(snapshot, selectedDraft)}
              </pre>
            )}
            <textarea
              className="registry-reason"
              placeholder="Release notes for the selected draft"
              value={draftNotes}
              onChange={(e) => setDraftNotes(e.target.value)}
              rows={2}
            />
            <div className="row" data-testid="registry-draft-actions">
              <button
                className="btn"
                disabled={!draftActions.canEditNotes}
                onClick={() => selectedDraft && command(() => bridge.registrySetDraftNotes(selectedDraft, draftNotes))}
              >
                Save notes
              </button>
              <button
                className="btn"
                disabled={!draftActions.canSubmit}
                onClick={() => selectedDraft && command(() => bridge.registrySubmitDraft(selectedDraft, false))}
              >
                Submit for review
              </button>
              <button
                className="btn"
                disabled={!draftActions.canBranch}
                title="The head stays published; the branch cannot be published until the lineage is resolved."
                onClick={() => selectedDraft && command(() => bridge.registrySubmitDraft(selectedDraft, true))}
              >
                Submit as branch
              </button>
              <button
                className="btn"
                disabled={!draftActions.canFromHead}
                onClick={() => selectedDraft && command(() => bridge.registryDraftFromHead(selectedDraft, keepDraftConfig))}
              >
                New draft from head
              </button>
              <label className="registry-check">
                <input type="checkbox" checked={keepDraftConfig} onChange={(e) => setKeepDraftConfig(e.target.checked)} />
                keep my config.json
              </label>
              <button
                className="btn"
                disabled={!draftActions.canDiscard}
                onClick={() => selectedDraft && command(() => bridge.registryDeleteDraft(selectedDraft), () => setSelectedDraft(null))}
              >
                Discard
              </button>
            </div>
            <div className="row" data-testid="registry-new-method">
              <select value={newProject} onChange={(e) => setNewProject(e.target.value)} disabled={!draftActions.canNewMethod}>
                <option value="">Project…</option>
                {projects.map((p) => (
                  <option key={p.id} value={p.id}>
                    {p.name}
                  </option>
                ))}
              </select>
              <input type="text" placeholder="New method name" value={newName} onChange={(e) => setNewName(e.target.value)} />
              <button
                className="btn"
                disabled={!draftActions.canNewMethod || !newProject || !newName.trim()}
                title="A new central method from the applied config.json (release notes from the box above)"
                onClick={() =>
                  command(() => bridge.registryNewMethodDraft(newProject, newName.trim(), draftNotes), () => setNewName(""))
                }
              >
                New method from current config
              </button>
            </div>
            <p className="registry-note">
              Drafts stay on this PC under your registry account until you submit them. A submitted revision is
              immutable and needs an independent review before it can be published.
            </p>
          </>
        )}
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
