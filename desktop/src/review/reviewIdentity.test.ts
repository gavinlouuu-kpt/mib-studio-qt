// Review ABI 5 (#783): the identity's E-LUT provenance as the shell decodes it
// from fetch_review_info / review_probe_runs JSON (src-tauri/src/review.rs
// run_identity): a snapshot file, a regenerated file of each provenance, a
// legacy file, a backend before ABI 5 and values the shell does not know.
import { describe, expect, it } from "vitest";
import { decodeReviewEModulusLutProvenance, type ReviewRunIdentity } from "./reviewBridge";
import { REVIEW_ABI_VERSION, REVIEW_EMODULUS_LUT_PROVENANCES } from "./reviewContract";

const LUT = {
  loaded: true,
  lut_id: "lut-v2",
  revision: "2",
  source: "r2-public-catalog",
  sha256: "41b9c0de",
  catalog_sha256: null,
  checksum_status: "verified",
};

/** The shell's identity JSON (review.rs serialises every member). */
function identity(json: string): ReviewRunIdentity {
  return JSON.parse(json) as ReviewRunIdentity;
}

function wire(members: Record<string, unknown>): string {
  return JSON.stringify({
    has_snapshot: false,
    snapshot_unreadable: false,
    snapshot_version: null,
    run_id: null,
    experiment_id: null,
    run_index: null,
    sample_ref: null,
    calibration_record_id: null,
    operator: null,
    annotation: null,
    annotation_unreadable: false,
    emodulus_lut: null,
    emodulus_lut_provenance: null,
    profile_id: null,
    ...members,
  });
}

describe("review identity E-LUT provenance (review ABI 5, #783)", () => {
  it("pins the contract", () => {
    expect(REVIEW_ABI_VERSION).toBeGreaterThanOrEqual(5); // 5 added the provenance; 6 is #716 A7a (runQuickLook.test.ts pins it)
    expect(REVIEW_EMODULUS_LUT_PROVENANCES).toEqual(["snapshot", "recorded", "host"]);
  });

  it("names the snapshot's own record", () => {
    const id = identity(
      wire({ has_snapshot: true, snapshot_version: 3, emodulus_lut: LUT, emodulus_lut_provenance: "snapshot", profile_id: "p" }),
    );
    expect(decodeReviewEModulusLutProvenance(id)).toBe("snapshot");
    expect(id.emodulus_lut?.lut_id).toBe("lut-v2");
  });

  it("names a regenerated file's record without a snapshot", () => {
    const recorded = identity(wire({ emodulus_lut: LUT, emodulus_lut_provenance: "recorded" }));
    expect(recorded.has_snapshot).toBe(false);
    expect(decodeReviewEModulusLutProvenance(recorded)).toBe("recorded");
    const host = identity(wire({ emodulus_lut: LUT, emodulus_lut_provenance: "host" }));
    expect(decodeReviewEModulusLutProvenance(host)).toBe("host");
    const none = identity(
      wire({ emodulus_lut: { ...LUT, loaded: false, lut_id: null, revision: null, source: null, sha256: null, checksum_status: null }, emodulus_lut_provenance: "recorded" }),
    );
    expect(none.emodulus_lut?.loaded).toBe(false);
    expect(decodeReviewEModulusLutProvenance(none)).toBe("recorded");
  });

  it("reads a legacy file, a closed session and an older backend as null", () => {
    expect(decodeReviewEModulusLutProvenance(identity(wire({})))).toBeNull();
    expect(decodeReviewEModulusLutProvenance(null)).toBeNull();
    expect(decodeReviewEModulusLutProvenance(undefined)).toBeNull();
    // A backend before review ABI 5 sends no member: unknown, never assumed "snapshot".
    const older = JSON.parse(wire({ has_snapshot: true, emodulus_lut: LUT })) as Record<string, unknown>;
    delete older.emodulus_lut_provenance;
    expect(decodeReviewEModulusLutProvenance(older as unknown as ReviewRunIdentity)).toBeNull();
  });

  it("never trusts an unknown value or a provenance without a record", () => {
    expect(decodeReviewEModulusLutProvenance(identity(wire({ emodulus_lut: LUT, emodulus_lut_provenance: "guessed" })))).toBeNull();
    expect(decodeReviewEModulusLutProvenance(identity(wire({ emodulus_lut: LUT, emodulus_lut_provenance: 1 })))).toBeNull();
    expect(decodeReviewEModulusLutProvenance(identity(wire({ emodulus_lut: null, emodulus_lut_provenance: "host" })))).toBeNull();
  });
});
