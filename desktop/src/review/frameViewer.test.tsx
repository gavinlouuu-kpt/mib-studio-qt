// @vitest-environment jsdom
// The frame viewer against the Qt FrameViewerDialog (#867): keys (REVIEW-026),
// info header (027), overlay combo and ROI Overlay check (028), zoom (029),
// Export as TIFF naming (031) and the series-count failure that used to be
// swallowed. The scheduler is a fake that runs the viewer's frame request, so
// the bridge call it makes (frame or series image) can be read.
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { FrameViewer, VIEWER_OVERLAYS, viewerInfo, type FrameViewerProps } from "./FrameViewer";
import { frameTiffName, withTiffExtension } from "./exports/frameExport";
import { WHEEL_STEP, ZOOM_MAX, ZOOM_MIN, ZOOM_STEP, wheelNotches, wheelZoom, zoomIn, zoomOut } from "./imageZoom";
import { OVERLAY, reviewBridge, type ReviewRow } from "./reviewBridge";

vi.mock("./reviewBridge", async (importOriginal) => {
  const real = await importOriginal<typeof import("./reviewBridge")>();
  return { ...real, packetToImageData: () => ({}) as ImageData };
});

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

const last = <T,>(items: T[]): T | undefined => items[items.length - 1];

const ROW: ReviewRow = {
  frame_index: "42", timestamp_ns: "123456789", valid: true, target_group: false, touches_border: false,
  has_single_inner_contour: true, in_range: true, in_channel: true, inner_contour_count: 2, object_id: 3,
  object_count: 1, track_id: 7, track_first_frame: "40", track_last_frame: "44", track_observation_count: 5,
  bbox_x: 1, bbox_y: 2, bbox_width: 10, bbox_height: 8, centroid_x: 5.25, centroid_y: 6.5, area: 120.04,
  area_um2: 30.01, deformability: 0.12345, area_ratio: 0.9123, ring_ratio: 18.5, laplacian_variance: null,
  youngs_modulus: 2.3456, brightness_q1: 1, brightness_q2: 2, brightness_q3: 3, brightness_q4: 4,
};

let root: Root;
let host: HTMLDivElement;
let draw: ((p: unknown) => void) | null;
const requested: string[] = [];
const logged: string[] = [];

function fakeScheduler() {
  return {
    mount: vi.fn((_slot: string, d: (p: unknown) => void) => {
      draw = d;
      return () => undefined;
    }),
    // Run the request the way the scheduler would, and note which call it made.
    request: vi.fn((_slot: string, fn: () => Promise<unknown>) => void fn()),
  };
}

function props(over: Partial<FrameViewerProps> = {}): FrameViewerProps {
  return {
    scheduler: fakeScheduler() as never,
    valid: true,
    index: 5,
    total: 10,
    overlay: OVERLAY.None,
    roiOverlay: false,
    recording: false,
    row: ROW,
    onIndexChange: vi.fn(),
    onOverlayChange: vi.fn(),
    onRoiOverlayChange: vi.fn(),
    onExportFrame: vi.fn(),
    onClose: vi.fn(),
    log: (l) => void logged.push(l),
    ...over,
  };
}

async function mount(p: FrameViewerProps) {
  await act(async () => root.render(<FrameViewer {...p} />));
  await act(async () => void (await Promise.resolve()));
}

const key = (k: string, init: KeyboardEventInit = {}) => act(async () => void window.dispatchEvent(new KeyboardEvent("keydown", { key: k, ...init })));
const button = (text: string) => {
  const b = Array.from(host.querySelectorAll("button")).find((x) => x.textContent?.trim() === text);
  if (!b) throw new Error(`no button ${text}`);
  return b;
};
const click = (el: Element) => act(async () => void el.dispatchEvent(new MouseEvent("click", { bubbles: true })));
const canvas = () => host.querySelector("canvas") as HTMLCanvasElement;
const header = () => host.querySelector(".viewer-info")?.textContent ?? "";

beforeEach(() => {
  requested.length = 0;
  logged.length = 0;
  draw = null;
  vi.spyOn(HTMLCanvasElement.prototype, "getContext").mockReturnValue({ putImageData: () => undefined } as never);
  vi.spyOn(reviewBridge, "seriesCount").mockResolvedValue(4);
  vi.spyOn(reviewBridge, "frame").mockImplementation(async (dataset, index, overlay, roi) => {
    requested.push(`frame ${dataset} ${index} ${overlay} ${roi}`);
    return undefined as never;
  });
  vi.spyOn(reviewBridge, "seriesFrame").mockImplementation(async (index, k, overlay, roi) => {
    requested.push(`series ${index} ${k} ${overlay} ${roi}`);
    return undefined as never;
  });
  host = document.createElement("div");
  document.body.append(host);
  root = createRoot(host);
});

afterEach(async () => {
  await act(async () => root.unmount());
  host.remove();
  vi.restoreAllMocks();
});

describe("REVIEW-026 keys", () => {
  it("Left and Up step to the previous frame, Right and Down to the next", async () => {
    const p = props();
    await mount(p);
    await key("ArrowLeft");
    await key("ArrowUp");
    await key("ArrowRight");
    await key("ArrowDown");
    expect((p.onIndexChange as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0])).toEqual([4, 4, 6, 6]);
  });

  it("wraps around both ends", async () => {
    const first = props({ index: 0 });
    await mount(first);
    await key("ArrowLeft");
    expect(first.onIndexChange).toHaveBeenLastCalledWith(9);
    const atEnd = props({ index: 9 });
    await mount(atEnd);
    await key("ArrowDown");
    expect(atEnd.onIndexChange).toHaveBeenLastCalledWith(0);
  });

  it("Esc closes", async () => {
    const p = props();
    await mount(p);
    await key("Escape");
    expect(p.onClose).toHaveBeenCalledTimes(1);
  });

  it("the series keeps its buttons and Shift+Up / Shift+Down, never moving the frame", async () => {
    const p = props();
    await mount(p);
    expect(host.textContent).toContain("Series 1/4");
    await key("ArrowDown", { shiftKey: true });
    expect(host.textContent).toContain("Series 2/4");
    await key("ArrowUp", { shiftKey: true });
    expect(host.textContent).toContain("Series 1/4");
    await click(button("Series Next >"));
    expect(host.textContent).toContain("Series 2/4");
    await click(button("< Series Prev"));
    expect(host.textContent).toContain("Series 1/4");
    expect(p.onIndexChange).not.toHaveBeenCalled();
  });

  it("series stops at the first and last image (Qt disables the buttons) and returns to the trigger on a new frame", async () => {
    const p = props();
    await mount(p);
    expect(button("< Series Prev").disabled).toBe(true);
    for (let i = 0; i < 6; i++) await key("ArrowDown", { shiftKey: true });
    expect(host.textContent).toContain("Series 4/4");
    expect(button("Series Next >").disabled).toBe(true);
    await mount({ ...p, index: 6 });
    expect(host.textContent).toContain("Series 1/4");
  });

  it("an arrow key aimed at the overlay combo is the combo's, not the viewer's", async () => {
    const p = props();
    await mount(p);
    const select = host.querySelector("select")!;
    await act(async () => void select.dispatchEvent(new KeyboardEvent("keydown", { key: "ArrowDown", bubbles: true })));
    expect(p.onIndexChange).not.toHaveBeenCalled();
  });
});

describe("series images", () => {
  it("the trigger image is the frame with the overlay; others are fetched raw with only the ROI", async () => {
    const p = props({ overlay: OVERLAY.AllMask, roiOverlay: true });
    await mount(p);
    expect(last(requested)).toBe("frame 0 5 3 true");
    await key("ArrowDown", { shiftKey: true });
    expect(last(requested)).toBe("series 5 1 0 true");
  });
});

describe("REVIEW-027 info header", () => {
  it("lists Qt's fields with Qt's labels and formats", () => {
    const fields = viewerInfo(ROW, null).map((f) => `${f.label}: ${f.value}`);
    expect(fields.slice(0, 8)).toEqual([
      "Frame Index: 42",
      "Timestamp: 123456789 ns",
      "Deformability: 0.123",
      "Area: 120.04",
      "Area Ratio: 0.912",
      "Ring Ratio: 18.500",
      "Valid: Yes",
      "Inner Count: 2",
    ]);
  });

  it("shows Valid: No for an invalid row", () => {
    expect(viewerInfo({ ...ROW, valid: false }, null).find((f) => f.label === "Valid")?.value).toBe("No");
  });

  it("names the series image the Qt way", () => {
    expect(viewerInfo(ROW, { position: 0, count: 4 }).find((f) => f.label === "Series")?.value).toBe("Trigger image (1/4)");
    expect(viewerInfo(ROW, { position: 2, count: 4 }).find((f) => f.label === "Series")?.value).toBe("Image 3/4 (metrics from trigger only)");
    expect(viewerInfo(ROW, null).some((f) => f.label === "Series")).toBe(false);
  });

  it("keeps what only YOFO Review shows after Qt's fields; a recording shows index and timestamp only", () => {
    const labels = viewerInfo({ ...ROW, target_group: true }, null).map((f) => f.label);
    expect(labels.slice(8)).toEqual(["Area (µm²)", "E (kPa)", "Object", "Track", "Target"]);
    expect(viewerInfo(ROW, { position: 1, count: 3 }, true).map((f) => f.label)).toEqual(["Frame Index", "Timestamp", "Series"]);
  });

  it("renders the header above the image, with the series indicator following the series position", async () => {
    await mount(props());
    expect(header()).toContain("Frame Index: 42 | Timestamp: 123456789 ns | Deformability: 0.123 | Area: 120.04 | Area Ratio: 0.912 | Ring Ratio: 18.500 | Valid: Yes | Inner Count: 2 | Series: Trigger image (1/4)");
    await key("ArrowDown", { shiftKey: true });
    expect(header()).toContain("Series: Image 2/4 (metrics from trigger only)");
  });
});

describe("REVIEW-028 overlay combo and ROI Overlay check", () => {
  it("offers the Qt viewer's overlay choices", () => {
    expect(VIEWER_OVERLAYS.map((o) => o.label)).toEqual(["None", "All contours", "Outer+inner (color)", "All mask", "Filtered mask"]);
    expect(VIEWER_OVERLAYS.map((o) => o.mode)).toEqual([OVERLAY.None, OVERLAY.AllContour, OVERLAY.OuterInnerColorCoded, OVERLAY.AllMask, OVERLAY.FilteredMask]);
  });

  it("shows the toolbar's overlay and ROI and reports a change without closing", async () => {
    const p = props({ overlay: OVERLAY.AllMask, roiOverlay: true });
    await mount(p);
    const select = host.querySelector("select") as HTMLSelectElement;
    expect(select.value).toBe(String(OVERLAY.AllMask));
    const roi = host.querySelector('input[type="checkbox"]') as HTMLInputElement;
    expect(roi.checked).toBe(true);
    expect(host.textContent).toContain("ROI Overlay");
    await act(async () => {
      select.value = String(OVERLAY.AllContour);
      select.dispatchEvent(new Event("change", { bubbles: true }));
    });
    expect(p.onOverlayChange).toHaveBeenCalledWith(OVERLAY.AllContour);
    await click(roi);
    expect(p.onRoiOverlayChange).toHaveBeenCalledWith(false);
    expect(p.onClose).not.toHaveBeenCalled();
  });

  it("asks the backend for the new overlay and ROI when the choice changes", async () => {
    const p = props({ overlay: OVERLAY.None });
    await mount(p);
    expect(last(requested)).toBe("frame 0 5 0 false");
    await mount({ ...p, overlay: OVERLAY.FilteredMask, roiOverlay: true });
    expect(last(requested)).toBe("frame 0 5 4 true");
  });

  it("disables both controls for a recording (no masks, no ROI)", async () => {
    await mount(props({ recording: true }));
    expect((host.querySelector("select") as HTMLSelectElement).disabled).toBe(true);
    expect((host.querySelector('input[type="checkbox"]') as HTMLInputElement).disabled).toBe(true);
  });
});

describe("REVIEW-029 zoom", () => {
  it("Zoom In multiplies by 1.2, Zoom Out divides by 1.2 and stops at 0.1", () => {
    expect(ZOOM_STEP).toBe(1.2);
    expect(zoomIn(1)).toBeCloseTo(1.2, 12);
    expect(zoomIn(zoomIn(1))).toBeCloseTo(1.44, 12);
    expect(zoomOut(1)).toBeCloseTo(1 / 1.2, 12);
    expect(zoomOut(0.11)).toBe(ZOOM_MIN);
    expect(zoomOut(ZOOM_MIN)).toBe(ZOOM_MIN);
    expect(zoomIn(ZOOM_MAX)).toBe(ZOOM_MAX);
  });

  it("Ctrl+wheel moves 10 % a notch within 0.1 – 10", () => {
    expect(WHEEL_STEP).toBe(0.1);
    expect(wheelNotches(-100, 0)).toBe(1);
    expect(wheelNotches(100, 0)).toBe(-1);
    expect(wheelNotches(-3, 1)).toBe(1);
    expect(wheelZoom(1, 1)).toBeCloseTo(1.1, 12);
    expect(wheelZoom(1, -1)).toBeCloseTo(0.9, 12);
    expect(wheelZoom(9.8, 1)).toBe(ZOOM_MAX);
    expect(wheelZoom(0.11, -1)).toBe(ZOOM_MIN);
  });

  async function drawn() {
    await mount(props());
    await act(async () => draw!({ valid: true, width: 100, height: 50 }));
  }

  it("sizes the canvas by the zoom and goes back to the window on Fit to Window", async () => {
    await drawn();
    expect(canvas().style.maxWidth).toBe("100%");
    await click(button("Zoom In"));
    expect(canvas().style.width).toBe("120px");
    expect(canvas().style.height).toBe("60px");
    await click(button("Zoom Out"));
    await click(button("Zoom Out"));
    expect(canvas().style.width).toBe("83px"); // 100 × 1.2 / 1.2 / 1.2 = 83.3
    await click(button("Fit to Window"));
    expect(canvas().style.maxWidth).toBe("100%");
    expect(canvas().style.width).toBe("");
    await click(button("1×"));
    expect(canvas().style.width).toBe("100px");
  });

  it("zooms with Ctrl+wheel only", async () => {
    await drawn();
    const wrap = host.querySelector(".viewer-image")!;
    const wheel = (init: WheelEventInit) => act(async () => void wrap.dispatchEvent(new WheelEvent("wheel", { bubbles: true, cancelable: true, ...init })));
    await wheel({ deltaY: -100 });
    expect(canvas().style.maxWidth).toBe("100%"); // plain wheel scrolls, never zooms
    await wheel({ deltaY: -100, ctrlKey: true });
    expect(canvas().style.width).toBe("110px");
    await wheel({ deltaY: 100, ctrlKey: true });
    await wheel({ deltaY: 100, ctrlKey: true });
    expect(canvas().style.width).toBe("89px"); // 1 × 1.1 × 0.9 × 0.9 = 0.891
  });
});

describe("REVIEW-031 Export as TIFF", () => {
  it("names the file like Qt and appends .tiff", () => {
    expect(frameTiffName("42", null)).toBe("frame_42.tiff");
    expect(frameTiffName("42", { position: 0 })).toBe("frame_42_series_0.tiff");
    expect(frameTiffName(7, { position: 3 })).toBe("frame_7_series_3.tiff");
    expect(withTiffExtension("/d/a")).toBe("/d/a.tiff");
    expect(withTiffExtension("/d/a.TIFF")).toBe("/d/a.TIFF");
    expect(withTiffExtension("/d/a.tif")).toBe("/d/a.tif");
    expect(withTiffExtension("/d/a.png")).toBe("/d/a.png.tiff");
  });

  it("hands the panel the frame's name and the displayed canvas as PNG", async () => {
    const p = props();
    await mount(p);
    HTMLCanvasElement.prototype.toBlob = function (cb: BlobCallback) {
      cb({ arrayBuffer: async () => new Uint8Array([0x89, 0x50, 0x4e, 0x47, 9]).buffer } as unknown as Blob);
    };
    await click(button("Export as TIFF..."));
    const [name, png] = (p.onExportFrame as ReturnType<typeof vi.fn>).mock.calls[0];
    expect(name).toBe("frame_42_series_0.tiff"); // the frame has a series: the trigger image is series 0
    expect(Array.from(await png())).toEqual([0x89, 0x50, 0x4e, 0x47, 9]);
    await key("ArrowDown", { shiftKey: true });
    await click(button("Export as TIFF..."));
    expect((p.onExportFrame as ReturnType<typeof vi.fn>).mock.calls[1][0]).toBe("frame_42_series_1.tiff");
  });

  it("is plain frame_N.tiff when the frame has no series, and waits for the metrics row", async () => {
    vi.spyOn(reviewBridge, "seriesCount").mockResolvedValue(0);
    const p = props();
    await mount(p);
    await click(button("Export as TIFF..."));
    expect((p.onExportFrame as ReturnType<typeof vi.fn>).mock.calls[0][0]).toBe("frame_42.tiff");
    await mount({ ...p, row: null });
    expect(button("Export as TIFF...").disabled).toBe(true);
  });
});

describe("series count failure", () => {
  it("is logged instead of swallowed, and the series controls stay hidden", async () => {
    vi.spyOn(reviewBridge, "seriesCount").mockRejectedValue(new Error("backend gone"));
    await mount(props());
    expect(logged).toEqual(["series count error: Error: backend gone"]);
    expect(host.textContent).not.toContain("Series ");
  });
});
