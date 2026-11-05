// @vitest-environment jsdom
// REVIEW-003 / REVIEW-004 (epic #867): Qt's "Cannot Open HDF5 File" critical box and its status label
// (HdfReviewTab.cpp:540-581, 598-728), as both Tauri surfaces show them.
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import {
  CORRUPT_FILE_DETAIL, MISSING_FILE_DETAIL, OPEN_FAILURE_TITLE, OpenFailureDialog, isMissingFileMessage, isOpenFailureMessage,
  openFailureFor, openFailureText, reviewStatusText,
} from "./reviewOpen";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
let host: HTMLDivElement, root: Root;
beforeEach(() => { host = document.createElement("div"); document.body.append(host); root = createRoot(host); });
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });

describe("open-failure wording", () => {
  it("uses Qt's title and the corrupt-file text with the h5recover hint", () => {
    expect(OPEN_FAILURE_TITLE).toBe("Cannot Open HDF5 File");
    expect(CORRUPT_FILE_DETAIL).toBe(
      "The file exists but its HDF5 metadata is corrupt, likely caused by an interrupted write (crash or forced close during an experiment).\n\n" +
      "Frame data may be partially recoverable using the h5recover tool from the HDF5 utilities package.",
    );
    expect(MISSING_FILE_DETAIL).toBe("File not found.");
  });

  it("tells a missing file from a corrupt one by the backend's message, as Qt does by QFile::exists", () => {
    for (const message of ["File not found: /data/a.h5", "Recording load failed: file not found"]) {
      expect(isMissingFileMessage(message)).toBe(true);
      expect(openFailureFor("/data/a.h5", message).detail).toBe(MISSING_FILE_DETAIL);
    }
    for (const message of ["Cannot read the HDF5 file (corrupt or not an HDF5 review file): /data/a.h5", "Recording load failed", "Failed to read /data/a.h5: bad alloc"]) {
      expect(isMissingFileMessage(message)).toBe(false);
      expect(openFailureFor("/data/a.h5", message).detail).toBe(CORRUPT_FILE_DETAIL);
    }
  });

  it("composes the body as Qt does: Failed to open, the path, a blank line, the detail", () => {
    expect(openFailureText({ path: "/data/a.h5", detail: MISSING_FILE_DETAIL })).toBe("Failed to open:\n/data/a.h5\n\nFile not found.");
  });

  it("treats only 'Recording load failed' as an open failure in MIB Studio, not a refusal about the instrument", () => {
    expect(isOpenFailureMessage("Recording load failed")).toBe(true);
    expect(isOpenFailureMessage("Recording load failed: file not found")).toBe(true);
    expect(isOpenFailureMessage("Experiment must be idle before loading review data")).toBe(false);
    expect(isOpenFailureMessage("Stop raw recording before loading review data")).toBe(false);
  });
});

describe("status label", () => {
  const experiment = { file_open: true, recording_file: false, total_valid: 250, total_invalid: 2 };
  it("shows Ready, Loading... and Error loading file", () => {
    expect(reviewStatusText("idle", null)).toBe("Ready");
    expect(reviewStatusText("idle", { ...experiment, file_open: false })).toBe("Ready");
    expect(reviewStatusText("loading", experiment)).toBe("Loading...");
    expect(reviewStatusText("error", null)).toBe("Error loading file");
  });
  it("shows the counts of the open file, with the accounting text Qt appends", () => {
    expect(reviewStatusText("idle", experiment)).toBe("Valid: 250, Invalid: 2");
    expect(reviewStatusText("idle", { ...experiment, total_valid: "7", total_invalid: "0" }, " · run completed")).toBe("Valid: 7, Invalid: 0 · run completed");
    expect(reviewStatusText("idle", { file_open: true, recording_file: true, total_valid: 1000, total_invalid: 0, filtered_frames: 3900 })).toBe("Recording: 1000 frames, 3900 empty skipped");
    expect(reviewStatusText("idle", { file_open: true, recording_file: true, total_valid: "12", total_invalid: "0" })).toBe("Recording: 12 frames, 0 empty skipped");
  });
});

describe("OpenFailureDialog", () => {
  it("is a modal alert with Qt's title and text, closed by OK or Escape", async () => {
    const close = vi.fn();
    await act(async () => root.render(<OpenFailureDialog failure={{ path: "/data/a.h5", detail: CORRUPT_FILE_DETAIL }} onClose={close} />));
    const dialog = host.querySelector<HTMLElement>('[role="alertdialog"]')!;
    expect(dialog.getAttribute("aria-modal")).toBe("true");
    expect(dialog.getAttribute("data-tour")).toBe("review-open-failure");
    expect(host.querySelector("h3")!.textContent).toBe("Cannot Open HDF5 File");
    expect(dialog.querySelector('[data-testid="open-failure-text"]')!.textContent).toBe(`Failed to open:\n/data/a.h5\n\n${CORRUPT_FILE_DETAIL}`);
    expect(document.activeElement).toBe(dialog.querySelector("button"));
    await act(async () => void dialog.querySelector("button")!.click());
    expect(close).toHaveBeenCalledTimes(1);
    await act(async () => void window.dispatchEvent(new KeyboardEvent("keydown", { key: "Escape" })));
    expect(close).toHaveBeenCalledTimes(2);
  });
});
