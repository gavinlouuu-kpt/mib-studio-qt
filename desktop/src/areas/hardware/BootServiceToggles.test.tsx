// @vitest-environment jsdom
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import contract from "../../../../crates/mib-bridge/contract/bridge-contract.json";
import {
  BOOT_DISABLED_SERVICES_KEY, BOOT_SERVICE_OPTIONS, BootServiceToggles, disabledServicesFrom, type PreferencesStore,
} from "./BootServiceToggles";

let host: HTMLDivElement, root: Root;
beforeEach(() => {
  Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });

const box = (label: string) => Array.from(host.querySelectorAll("label")).find((l) => l.textContent === label)!.querySelector("input")!;
const save = () => Array.from(host.querySelectorAll("button")).find((b) => b.textContent === "Save")!;
const flush = () => act(async () => { await Promise.resolve(); });
const fake = (stored: Record<string, unknown>) => {
  let document = stored;
  const store: PreferencesStore & { writes: Record<string, unknown>[] } = {
    writes: [],
    get: vi.fn(async () => structuredClone(document)),
    set: vi.fn(async (next) => { document = next; store.writes.push(structuredClone(next)); }),
  };
  return store;
};
const mount = async (store: PreferencesStore) => { await act(async () => root.render(<BootServiceToggles store={store} />)); await flush(); };

describe("Boot Service Toggles (HW-038)", () => {
  it("lists Qt's services with Qt's labels and the explanation", async () => {
    await mount(fake({}));
    expect(Array.from(host.querySelectorAll("label")).map((l) => l.textContent)).toEqual([
      "SQLite service", "HDF5 service", "Processing service", "Autofocus wiring", "Trigger wiring",
      "Capture service", "Playback service", "Auto update checker"]);
    expect(host.textContent).toContain("Disable selected services at next application startup. Changes are persisted and applied on next launch.");
    expect(Array.from(host.querySelectorAll("input")).every((i) => !i.checked)).toBe(true);
    expect(host.textContent).not.toContain("Saved.");
  });

  it("starts from the persisted choice, ignoring unknown tokens", async () => {
    await mount(fake({ [BOOT_DISABLED_SERVICES_KEY]: ["Capture", " auto-update ", "bogus", 7] }));
    expect(box("Capture service").checked).toBe(true);
    expect(box("Auto update checker").checked).toBe(true);
    expect(box("SQLite service").checked).toBe(false);
  });

  it("saves the tokens in Qt's order, keeps every other preference, and says to restart", async () => {
    const store = fake({ sidebarWidth: 280, theme: "dark", [BOOT_DISABLED_SERVICES_KEY]: ["sqlite"] });
    await mount(store);
    await act(async () => box("Playback service").click());
    await act(async () => box("SQLite service").click()); // off again
    await act(async () => box("Autofocus wiring").click());
    await act(async () => save().click());
    await flush();
    expect(store.writes).toEqual([{ sidebarWidth: 280, theme: "dark", bootDisabledServices: ["autofocus", "playback"] }]);
    expect(host.querySelector('[role="status"]')!.textContent).toBe("Saved. Restart the application for changes to take effect.");
    // Editing again withdraws the note: it is about what was saved.
    await act(async () => box("HDF5 service").click());
    expect(host.querySelector('[role="status"]')).toBeNull();
  });

  it("reads the preferences again at save time, so a key written meanwhile survives", async () => {
    let document: Record<string, unknown> = { a: 1 };
    const store: PreferencesStore = {
      get: async () => structuredClone(document),
      set: async (next) => { document = next; },
    };
    await mount(store);
    document = { a: 1, writtenLater: true }; // another part of the app saved while the form was open
    await act(async () => box("Trigger wiring").click());
    await act(async () => save().click());
    await flush();
    expect(document).toEqual({ a: 1, writtenLater: true, bootDisabledServices: ["trigger"] });
  });

  it("saves an empty choice as an empty list, which re-enables everything", async () => {
    const store = fake({ [BOOT_DISABLED_SERVICES_KEY]: ["hdf5"] });
    await mount(store);
    await act(async () => box("HDF5 service").click());
    await act(async () => save().click());
    await flush();
    expect(store.writes).toEqual([{ bootDisabledServices: [] }]);
  });

  it("reports a failed read or write and writes nothing it could not base on the stored document", async () => {
    const unreadable: PreferencesStore = { get: async () => { throw new Error("preferences.json is malformed"); }, set: vi.fn() };
    await mount(unreadable);
    expect(host.querySelector('[role="alert"]')!.textContent).toContain("Could not read the saved preferences: preferences.json is malformed");
    expect(save().disabled).toBe(true); // not loaded: no blind overwrite
    expect(unreadable.set).not.toHaveBeenCalled();

    await act(async () => root.render(<div />));
    const failing: PreferencesStore = { get: async () => ({}), set: vi.fn(async () => { throw new Error("disk full"); }) };
    await mount(failing);
    await act(async () => save().click());
    await flush();
    expect(host.querySelector('[role="alert"]')!.textContent).toBe("Could not save: disk full");
    expect(host.textContent).not.toContain("Saved.");
  });

  it("offers exactly the tokens the contract lists for the shell", () => {
    expect(BOOT_SERVICE_OPTIONS.map((o) => o.token)).toEqual(contract.hardware_parity.boot_service_tokens);
    expect([...disabledServicesFrom(["playback", "sqlite", "nope"])]).toEqual(["playback", "sqlite"]);
  });
});
