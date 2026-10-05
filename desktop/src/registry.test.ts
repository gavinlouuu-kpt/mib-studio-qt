// Central profile registry view model (bridge schema v24, #398): the same
// truth the Qt dialog shows — connectivity separate from the account, each
// central state as itself, revoked marked, button enablement, and warnings.
import { describe, expect, it } from "vitest";
import type { RegistryRevision, RegistrySnapshot } from "./bridge";
import {
  REGISTRY_CENTRAL_STATES,
  REGISTRY_CONNECTIVITY,
  REGISTRY_JOB_KINDS,
  REGISTRY_JOB_STATES,
  REGISTRY_SESSION_STATES,
} from "./bridgeContract";
import { CENTRAL_STATE_NOTE, centralStateText, changed, jobText, toRegistryView } from "./registry";

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
});
