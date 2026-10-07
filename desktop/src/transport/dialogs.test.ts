// @vitest-environment jsdom
import {afterEach, expect, it, vi} from "vitest";
import * as dialog from "@tauri-apps/plugin-dialog";
import {confirm} from "./dialogs";
vi.mock("@tauri-apps/plugin-dialog", () => ({confirm: vi.fn()}));
vi.mock("./index", () => ({isRemote: false}));
afterEach(() => {
  Reflect.deleteProperty(window, "__TAURI_INTERNALS__");
  vi.restoreAllMocks();
  vi.resetAllMocks();
});
it.each([true, false])("awaits native confirmation resolving %s", async value => {
  Object.assign(window, {__TAURI_INTERNALS__: {}});
  const browser = vi.spyOn(window, "confirm");
  vi.mocked(dialog.confirm).mockResolvedValue(value);
  expect(await confirm("Proceed?", {kind: "warning"})).toBe(value);
  expect(dialog.confirm).toHaveBeenCalledWith("Proceed?", {kind: "warning"});
  expect(browser).not.toHaveBeenCalled();
});
it("fails closed when the native command is rejected", async () => {
  Object.assign(window, {__TAURI_INTERNALS__: {}});
  vi.mocked(dialog.confirm).mockRejectedValue(new Error("Command not allowed"));
  expect(await confirm("Proceed?")).toBe(false);
});
it.each([true, false])("uses browser confirmation resolving %s", async value => {
  vi.spyOn(window, "confirm").mockReturnValue(value);
  expect(await confirm("Proceed?")).toBe(value);
  expect(dialog.confirm).not.toHaveBeenCalled();
});
it("awaits an async browser shim cancellation instead of treating its Promise as true", async () => {
  vi.spyOn(window, "confirm").mockImplementation(() => Promise.resolve(false) as unknown as boolean);
  expect(await confirm("Proceed?")).toBe(false);
});
it("fails closed if browser confirmation throws", async () => {
  vi.spyOn(window, "confirm").mockImplementation(() => {throw new Error("Unavailable");});
  expect(await confirm("Proceed?")).toBe(false);
});
