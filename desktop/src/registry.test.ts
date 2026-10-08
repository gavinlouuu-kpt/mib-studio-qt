// Central profile registry view model (bridge schema v25, #398): the
// backend truth — connectivity separate from the account, each
// central state as itself, revoked marked, button enablement, and warnings.
import { describe, expect, it } from "vitest";
import type { RegistryRevision, RegistrySnapshot } from "./bridge";
import {
  REGISTRY_CENTRAL_STATES,
  REGISTRY_CONNECTIVITY,
  REGISTRY_JOB_KINDS,
  REGISTRY_JOB_STATES,
  REGISTRY_LOCAL_VALIDATION,
  REGISTRY_SESSION_STATES,
} from "./bridgeContract";
import type { RegistryDraft } from "./bridge";
import {
  APPLY_HINT,
  APPLY_NEEDS_FILES,
  applyConfirmText,
  applyResultText,
  CENTRAL_STATE_NOTE,
  actionsFor,
  conflictText,
  detailLines,
  draftActionsFor,
  draftRows,
  reviewActionsFor,
  centralStateText,
  changed,
  instrumentText,
  jobText,
  localValidationText,
  toRegistryView,
} from "./registry";

function revision(partial: Partial<RegistryRevision>): RegistryRevision {
  return {
    revision_id: "r1",
    method_id: "m1",
    project_id: "p1",
    display_name: "Cell Sorting",
    author_id: "alice",
    content_hash: "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
    revision_number: "12",
    metadata_version: "1",
    central_state: REGISTRY_CENTRAL_STATES.Published,
    materialized_dir: "",
    local_validation: REGISTRY_LOCAL_VALIDATION.None,
    validated_by: "",
    validated_at_utc: "",
    parent_revision_id: "",
    release_notes: "",
    newer_revision_id: "",
    ...partial,
  };
}

function snapshot(partial: Partial<RegistrySnapshot> = {}): RegistrySnapshot {
  return {
    valid: true,
    configured: true,
    generation: "1",
    origin: "https://registry.example",
    session: REGISTRY_SESSION_STATES.SignedOut,
    subject_id: "",
    email: "",
    connectivity: REGISTRY_CONNECTIVITY.Unknown,
    health_message: "",
    successful_requests: "0",
    failed_requests: "0",
    rejected_revisions: "0",
    projects: [],
    revisions: [],
    corrupt_revision_ids: [],
    cache_error: "",
    has_last_successful_refresh: false,
    last_successful_refresh_unix_ms: 0,
    last_job: { job_id: "0", kind: 0, state: 0, message: "" },
    queued_jobs: "0",
    busy: false,
    instrument_id: "123e4567-e89b-42d3-a456-426614174000",
    instrument_name: "MIB-01",
    drafts: [],
    methods: [],
    history_revision_id: "",
    history: [],
    submit_conflict: {
      present: false,
      draft_id: "",
      base_revision_id: "",
      head_revision_id: "",
      compared: false,
      upstream_changes: [],
      draft_vs_head: [],
    },
    ...partial,
  };
}

describe("registry view model", () => {
  it("says plainly when the registry is not configured and disables everything", () => {
    const v = toRegistryView(snapshot({ configured: false }));
    expect(v.status).toContain("not configured");
    expect([v.showSignIn, v.canSignIn, v.canRefresh, v.canCancel, v.canSignOut]).toEqual([
      false, false, false, false, false,
    ]);
  });

  it("signed out: sign-in offered, refresh needs a session", () => {
    const v = toRegistryView(snapshot());
    expect(v.account).toBe("Not signed in");
    expect(v.showSignIn && v.canSignIn).toBe(true);
    expect(v.canRefresh || v.canSignOut).toBe(false);
  });

  it("signed in: rows with project names, each central state as itself, revoked marked", () => {
    const v = toRegistryView(
      snapshot({
        session: REGISTRY_SESSION_STATES.SignedIn,
        email: "bob@lab",
        connectivity: REGISTRY_CONNECTIVITY.Online,
        projects: [{ project_id: "p1", display_name: "Cell Sorting Team", roles: ["operator"] }],
        revisions: [
          revision({ revision_id: "a", central_state: REGISTRY_CENTRAL_STATES.Published }),
          revision({ revision_id: "b", revision_number: "11", central_state: REGISTRY_CENTRAL_STATES.Superseded }),
          revision({ revision_id: "c", revision_number: "13", central_state: REGISTRY_CENTRAL_STATES.Revoked }),
        ],
      }),
    );
    expect(v.status).toBe("Central registry: online");
    expect(v.account).toBe("Signed in as bob@lab");
    expect(v.showSignIn).toBe(false);
    expect(v.canRefresh && v.canSignOut).toBe(true);
    expect(v.rows.map((r) => r.state)).toEqual(["Published", "Superseded", "REVOKED - do not use"]);
    expect(v.rows.map((r) => r.revoked)).toEqual([false, false, true]);
    expect(v.rows[0]).toMatchObject({ project: "Cell Sorting Team", revision: "r12", hashPrefix: "0123456789ab" });
    expect(v.email).toBe("bob@lab");
  });

  it("offline cache after a restart: listed, labelled, refresh needs sign-in", () => {
    const v = toRegistryView(
      snapshot({
        session: REGISTRY_SESSION_STATES.CachedOffline,
        email: "bob@lab",
        connectivity: REGISTRY_CONNECTIVITY.Offline,
        health_message: "Registry temporarily unavailable",
        revisions: [revision({})],
      }),
    );
    expect(v.status).toContain("offline");
    expect(v.account).toContain("Cached methods for bob@lab");
    expect(v.rows).toHaveLength(1);
    expect(v.canRefresh).toBe(false);
    expect(v.showSignIn && v.canSignOut).toBe(true);
    expect(v.warnings).toContain("Registry temporarily unavailable");
  });

  it("busy: cancel offered, other actions held", () => {
    const v = toRegistryView(snapshot({ session: REGISTRY_SESSION_STATES.SignedIn, busy: true }));
    expect(v.canCancel).toBe(true);
    expect(v.canRefresh || v.canSignIn || v.canSignOut).toBe(false);
    expect(v.activity).toContain("Working...");
  });

  it("surfaces cache errors, hidden corrupt rows and rejected revisions", () => {
    const v = toRegistryView(
      snapshot({ cache_error: "Profile cache unreadable", corrupt_revision_ids: ["x", "y"], rejected_revisions: "3" }),
    );
    expect(v.warnings).toEqual([
      "Profile cache unreadable",
      "2 cached revision(s) failed integrity checks and are hidden.",
      "3 registry revision(s) failed verification and were not cached.",
    ]);
  });

  it("describes the last job and every central state", () => {
    expect(
      jobText({ job_id: "7", kind: REGISTRY_JOB_KINDS.SignIn, state: REGISTRY_JOB_STATES.Failed, message: "rejected" }),
    ).toBe("Last action: sign in failed - rejected");
    expect(jobText({ job_id: "0", kind: 0, state: 0, message: "" })).toBe("");
    for (const value of Object.values(REGISTRY_CENTRAL_STATES)) {
      expect(centralStateText(value)).not.toMatch(/^Unknown/);
    }
    expect(CENTRAL_STATE_NOTE).toContain("not local validation");
  });

  it("re-renders only on generation, busy or validity changes", () => {
    const a = snapshot();
    expect(changed(null, a)).toBe(true);
    expect(changed(a, { ...a })).toBe(false);
    expect(changed(a, { ...a, generation: "2" })).toBe(true);
    expect(changed(a, { ...a, busy: true })).toBe(true);
  });

  it("M2b: shows local validation per row and the instrument", () => {
    const v = toRegistryView(
      snapshot({
        session: REGISTRY_SESSION_STATES.SignedIn,
        revisions: [
          revision({
            revision_id: "a",
            local_validation: REGISTRY_LOCAL_VALIDATION.Passed,
            validated_by: "user-bob",
            validated_at_utc: "2026-10-04 10:00:00",
            materialized_dir: "/data/methods/a",
          }),
          revision({ revision_id: "b", local_validation: REGISTRY_LOCAL_VALIDATION.Failed, validated_by: "x" }),
          revision({ revision_id: "c" }),
        ],
      }),
    );
    expect(v.rows.map((r) => r.validation)).toEqual([
      "Validated here by user-bob (2026-10-04 10:00:00 UTC)",
      "Validation FAILED here (x,  UTC)",
      "Not validated here",
    ]);
    expect(v.rows.map((r) => r.validationFailed)).toEqual([false, true, false]);
    expect(v.rows.map((r) => r.materialized)).toEqual([true, false, false]);
    expect(v.instrument).toBe("Instrument: MIB-01 (123e4567…)");
    expect(instrumentText(snapshot({ instrument_id: "" }))).toContain("unknown");
    expect(instrumentText(snapshot({ instrument_name: "" }))).toBe("Instrument: 123e4567…");
    expect(localValidationText(revision({}))).toBe("Not validated here");
  });

  it("M2b/M2c: actions follow selection, session, central state and materialization", () => {
    const signedIn = snapshot({
      session: REGISTRY_SESSION_STATES.SignedIn,
      revisions: [
        revision({ revision_id: "pub", materialized_dir: "/data/methods/pub" }),
        revision({ revision_id: "raw" }),
        revision({ revision_id: "rev", central_state: REGISTRY_CENTRAL_STATES.Revoked }),
        revision({ revision_id: "sub", central_state: REGISTRY_CENTRAL_STATES.Superseded }),
      ],
    });
    const view = toRegistryView(signedIn);
    expect(actionsFor(view, signedIn, null)).toMatchObject({ canMaterialize: false, canMarkValidated: false });
    expect(actionsFor(view, signedIn, "pub")).toEqual({
      canMaterialize: true,
      canMarkValidated: true,
      canApply: true,
      applyReason: APPLY_HINT,
    });
    expect(actionsFor(view, signedIn, "raw")).toMatchObject({ canApply: false, applyReason: APPLY_NEEDS_FILES });
    expect(actionsFor(view, signedIn, "sub")).toMatchObject({ canMaterialize: true, canMarkValidated: true });
    expect(actionsFor(view, signedIn, "rev")).toMatchObject({ canMaterialize: false, canMarkValidated: false, canApply: false });

    const offline = { ...signedIn, session: REGISTRY_SESSION_STATES.CachedOffline };
    expect(actionsFor(toRegistryView(offline), offline, "pub")).toMatchObject({
      canMaterialize: true,
      canMarkValidated: false,
    });
    const busy = { ...signedIn, busy: true };
    expect(actionsFor(toRegistryView(busy), busy, "pub")).toMatchObject({ canMaterialize: false, canApply: false });
    const noInstrument = { ...signedIn, instrument_id: "" };
    expect(actionsFor(toRegistryView(noInstrument), noInstrument, "pub").canMarkValidated).toBe(false);
  });

  it("M2c: Apply confirmation and outcome wording", () => {
    const plan = {
      ok: true,
      error: "",
      revision_id: "r2",
      display_name: "Cell Sorting",
      revision_number: "2",
      central_state: "published",
      changed_keys: ["gain", "image_processing.filters.enable_border_check"],
      camera_script_path: "/data/methods/r2/egrabberConfig.js",
    };
    const text = applyConfirmText(plan);
    expect(text).toContain('Apply "Cell Sorting" r2 (published)?');
    expect(text).toContain("  • gain");
    expect(text).toContain("travel with the method");
    expect(text).toContain("/data/methods/r2/egrabberConfig.js");
    expect(applyConfirmText({ ...plan, changed_keys: [] })).toContain("byte-for-byte");
    expect(applyConfirmText({ ...plan, ok: false, error: "not materialized" })).toBe("Cannot apply: not materialized");
    const many = applyConfirmText({ ...plan, changed_keys: Array.from({ length: 30 }, (_, i) => `k${i}`) });
    expect(many).toContain("... and 5 more");
    expect(applyResultText({ ok: true, error: "", applied: ["image_processing", "roi"], not_applied: [] })).toBe(
      "Applied (image_processing, roi).",
    );
    expect(
      applyResultText({ ok: true, error: "", applied: ["image_processing"], not_applied: ["dot_grid (Qt shell only)"] }),
    ).toContain("Not applicable in this app: dot_grid (Qt shell only).");
    expect(applyResultText({ ok: false, error: "An experiment is in progress", applied: [], not_applied: [] })).toBe(
      "Apply failed: An experiment is in progress",
    );
    // The backend names the field and its bound; the panel shows it, not a generic failure.
    expect(
      applyResultText({
        ok: false,
        error: "buffer_threshold must be between 1 and 10000000 (got 0)",
        applied: [],
        not_applied: [],
      }),
    ).toBe("Apply failed: buffer_threshold must be between 1 and 10000000 (got 0)");
    // A pending ROI is not "not applicable": it is applied on the first frame.
    const pending = applyResultText({
      ok: true,
      error: "",
      applied: ["image_processing"],
      not_applied: ["roi (pending: applied on the first captured frame)", "dot_grid (Qt shell only)"],
    });
    expect(pending).toContain("The ROI is applied on the first captured frame");
    expect(pending).toContain("Not applicable in this app: dot_grid (Qt shell only).");
    expect(pending).not.toContain("Not applicable in this app: roi");
  });

  it("M2b: a local validation change re-renders without a generation bump", () => {
    const a = snapshot({ revisions: [revision({})] });
    const b = { ...a, revisions: [revision({ local_validation: REGISTRY_LOCAL_VALIDATION.Passed })] };
    expect(changed(a, b)).toBe(true);
    expect(changed(a, { ...a, revisions: [revision({})] })).toBe(false);
  });

  const draft = (partial: Partial<RegistryDraft>): RegistryDraft => ({
    draft_id: "d1",
    project_id: "p1",
    method_id: "m1",
    new_method: false,
    method_display_name: "Cell Sorting",
    base_revision_id: "r1",
    release_notes: "notes",
    submitted_revision_id: "",
    updated_at_utc: "2026-10-04 10:00:00",
    ...partial,
  });
  const project = (roles: string[]) => [{ project_id: "p1", display_name: "Team", roles }];

  it("M3b: review actions follow role and central state", () => {
    const base = {
      session: REGISTRY_SESSION_STATES.SignedIn,
      revisions: [
        revision({ revision_id: "sub", central_state: REGISTRY_CENTRAL_STATES.Submitted }),
        revision({ revision_id: "app", central_state: REGISTRY_CENTRAL_STATES.Approved }),
        revision({ revision_id: "pub", central_state: REGISTRY_CENTRAL_STATES.Published }),
        revision({ revision_id: "arc", central_state: REGISTRY_CENTRAL_STATES.Archived }),
      ],
    };
    const author = snapshot({ ...base, projects: project(["author"]) });
    expect(reviewActionsFor(author, "sub")).toMatchObject({ canApprove: false, canNewDraft: true, canHistory: true });
    const reviewer = snapshot({ ...base, projects: project(["reviewer"]) });
    expect(reviewActionsFor(reviewer, "sub")).toMatchObject({ canApprove: true, canReject: true, canPublish: false });
    expect(reviewActionsFor(reviewer, "pub").canApprove).toBe(false);
    const publisher = snapshot({ ...base, projects: project(["publisher"]) });
    expect(reviewActionsFor(publisher, "app").canPublish).toBe(true);
    expect(reviewActionsFor(publisher, "pub")).toMatchObject({ canArchive: true, canRevoke: true, canPublish: false });
    expect(reviewActionsFor(publisher, "arc")).toMatchObject({ canArchive: false, canRevoke: true });
    const admin = snapshot({ ...base, projects: project(["admin"]) });
    expect(reviewActionsFor(admin, "sub").canApprove).toBe(true);
    const offline = snapshot({ ...base, session: REGISTRY_SESSION_STATES.CachedOffline, projects: project(["admin"]) });
    expect(reviewActionsFor(offline, "sub")).toMatchObject({ canApprove: false, canNewDraft: true, canHistory: false });
    expect(reviewActionsFor(admin, null).canNewDraft).toBe(false);
  });

  it("M3b: drafts, statuses and the conflict choices", () => {
    const s = snapshot({
      session: REGISTRY_SESSION_STATES.SignedIn,
      projects: project(["author"]),
      revisions: [
        revision({ revision_id: "r1", revision_number: "1" }),
        revision({ revision_id: "r2", revision_number: "2" }),
        revision({ revision_id: "r3", revision_number: "3", central_state: REGISTRY_CENTRAL_STATES.Submitted }),
      ],
      drafts: [
        draft({ draft_id: "open" }),
        draft({ draft_id: "new", new_method: true, base_revision_id: "" }),
        draft({ draft_id: "done", submitted_revision_id: "r3" }),
        draft({ draft_id: "stale" }),
      ],
      submit_conflict: {
        present: true,
        draft_id: "stale",
        base_revision_id: "r1",
        head_revision_id: "r2",
        compared: true,
        upstream_changes: ["gain"],
        draft_vs_head: ["gain", "image_processing.threshold"],
      },
    });
    expect(draftRows(s).map((r) => [r.base, r.status])).toEqual([
      ["r1", "Draft (not submitted)"],
      ["new method", "Draft (not submitted)"],
      ["r1", "Submitted as r3"],
      ["r1", "CONFLICT: head is now r2"],
    ]);
    expect(draftActionsFor(s, "open")).toMatchObject({ canSubmit: true, canBranch: false, canFromHead: false, canNewMethod: true });
    expect(draftActionsFor(s, "done")).toMatchObject({ canSubmit: false, canEditNotes: false, canDiscard: true });
    expect(draftActionsFor(s, "stale")).toMatchObject({ canSubmit: false, canBranch: true, canFromHead: true });
    const text = conflictText(s, "stale");
    expect(text).toContain("based on r1, but r2 has been published");
    expect(text).toContain("Nothing was sent");
    expect(text).toContain("Changed upstream: gain");
    expect(text).toContain("image_processing.threshold");
    expect(conflictText(s, "open")).toBe("");
    const viewer = { ...s, projects: project(["viewer"]) };
    expect(draftActionsFor(viewer, "open")).toMatchObject({ canSubmit: false, canEditNotes: true, canNewMethod: false });
  });

  it("M3b: update available, lineage, notes and history", () => {
    const s = snapshot({
      revisions: [
        revision({ revision_id: "r1", revision_number: "1", newer_revision_id: "r2", release_notes: "first" }),
        revision({ revision_id: "r2", revision_number: "2", parent_revision_id: "r1" }),
      ],
      history_revision_id: "r2",
      history: [
        { who: "carol", what: "approved", reason: "ok", created_at: "t1", review: true },
        { who: "alice", what: "submitted", reason: "", created_at: "t0", review: false },
      ],
    });
    expect(toRegistryView(s).rows.map((r) => r.update)).toEqual(["r2 available", ""]);
    expect(detailLines(s, "r1")).toEqual([
      "Cell Sorting r1 · Published · parent none",
      "Update available: r2 is published",
      "Release notes: first",
    ]);
    expect(detailLines(s, "r2")).toEqual([
      "Cell Sorting r2 · Published · parent r1",
      "Release notes: (none)",
      "Review: approved by carol at t1 - ok",
      "Event: submitted by alice at t0",
    ]);
  });
});
