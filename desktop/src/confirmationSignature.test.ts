import { describe, expect, it } from "vitest";
import { deviceIdentity, preflightSignature, type SignatureFacts } from "./confirmationSignature";

const BASE: SignatureFacts = {
  camera: { configured: true, mode: 0, label: "Camera 01 / EGrabber" },
  coreVersion: "2.1.0",
  plCore: { buildId: "76aea765", weightsId: "eea09a3f" },
  samplePump: { connected: true, identity: "tushui@/dev/ttyPS1#3" },
  sheathPump: { connected: true, identity: "tushui@/dev/ttyPS1#4" },
  zStage: { connected: true, identity: "ZC300 SN42 @ COM5" },
  nanopositioner: { connected: true, identity: "PI E-709 COM7" },
  profile: { id: "cell-mechanics", revision: "r3" },
  storagePath: "/mnt/sata/yofo",
};

describe("Preflight confirmation signature (#713)", () => {
  it("is empty with no camera configured, so nothing can be confirmed", () => {
    expect(preflightSignature({ ...BASE, camera: { ...BASE.camera, configured: false } })).toBe("");
  });

  it("is stable for the same identities", () => {
    expect(preflightSignature({ ...BASE })).toBe(preflightSignature(structuredClone(BASE)));
  });

  it("changes when any bound identity changes", () => {
    const base = preflightSignature(BASE);
    const changes: Partial<SignatureFacts>[] = [
      { camera: { ...BASE.camera, label: "Camera 02" } },
      { camera: { ...BASE.camera, mode: 1 } },
      { coreVersion: "2.2.0" },
      { plCore: { buildId: "00000000", weightsId: "eea09a3f" } },
      { plCore: { buildId: "76aea765", weightsId: "11111111" } },
      { samplePump: { connected: true, identity: "tushui@/dev/ttyPS1#5" } },
      { sheathPump: { connected: false, identity: "" } },
      { zStage: { connected: true, identity: "ZC300 SN43 @ COM5" } },
      { nanopositioner: { connected: false, identity: "" } },
      { profile: { id: "cell-mechanics", revision: "r4" } },
      { profile: { id: "beads", revision: "r3" } },
      { profile: null },
      { storagePath: "/var/lib/yofo-studio" },
    ];
    const signatures = changes.map((c) => preflightSignature({ ...BASE, ...c }));
    for (const s of signatures) expect(s).not.toBe(base);
    expect(new Set(signatures).size).toBe(signatures.length);
  });

  it("tells a device not read yet from one read and absent", () => {
    const unread = preflightSignature({ ...BASE, samplePump: null });
    const absent = preflightSignature({ ...BASE, samplePump: { connected: false, identity: "" } });
    expect(unread).not.toBe(absent);
    expect(preflightSignature({ ...BASE, storagePath: null })).not.toBe(preflightSignature({ ...BASE, storagePath: "" }));
  });

  it("cannot be confused by separators inside values", () => {
    const a = preflightSignature({ ...BASE, camera: { ...BASE.camera, label: "a|core:b" }, coreVersion: "c" });
    const b = preflightSignature({ ...BASE, camera: { ...BASE.camera, label: "a" }, coreVersion: "b|c" });
    expect(a).not.toBe(b);
  });

  it("keeps the camera identity used by the Alignment confirmation unchanged", () => {
    expect(deviceIdentity(BASE.camera)).toBe("0|Camera 01 / EGrabber");
    expect(deviceIdentity({ ...BASE.camera, configured: false })).toBe("");
  });
});
