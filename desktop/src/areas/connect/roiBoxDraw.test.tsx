// @vitest-environment jsdom
// #863 A2 (CONNECT-047, 049): the yellow camera-window box drawn over the live Overview frame (areas/preview/usePreview.ts draw)
// follows the ROI Overlay toggle and is mirrored for a MindVision profile that flips the image.
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import type { CameraGeometry } from "../../bridge";

vi.mock("../../bridge", async (importOriginal) => ({
  ...(await importOriginal<typeof import("../../bridge")>()),
  mono8ToImageData: () => ({ data: new Uint8ClampedArray(4), width: 1, height: 1 }),
}));
import { useLiveFrames } from "../preview/usePreview";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

const geometry = (over: Partial<CameraGeometry> = {}): CameraGeometry => ({
  supported: true, overview: true, camera: "mindvision", sensor_width: 816, sensor_height: 624, roi: { x: 100, y: 120, width: 256, height: 128 },
  width_increment: 1, height_increment: 1, offset_x_increment: 1, offset_y_increment: 1, min_width: 16, min_height: 4, session: {},
  flip_horizontal: false, flip_vertical: false, ...over,
});

let host: HTMLDivElement, root: Root;
let deliver: (packet: unknown) => void;
let ctx: { putImageData: ReturnType<typeof vi.fn>; strokeRect: ReturnType<typeof vi.fn>; strokeStyle: string; lineWidth: number };
let canvas: { width: number; height: number; getContext: () => unknown };
let geometryRef: { current: CameraGeometry | null }, windowRef: { current: { x: number; y: number; width: number; height: number } | null };
let overlayRef: { current: boolean };

function Harness() {
  useLiveFrames({
    tab: "overview", tabRef: { current: "overview" }, instrumentModesRef: { current: false }, append: () => undefined,
    previewCanvasRef: { current: null }, framePulls: { current: { mount: (id: string, d: (p: unknown) => void) => { if (id === "live") deliver = d; return () => undefined; }, invalidate: () => undefined } },
    lastMetadataRenderMs: { current: 0 }, setLastMeta: () => undefined,
    ratesRef: { current: { windowStartMs: 0, frames: 0, bytes: 0, displayFps: 0, dataRateMBs: 0, lastFrameIndex: null } }, setRatesTick: () => undefined,
    previewLoopRef: { current: null }, liveCanvasRef: { current: canvas }, cameraGeometryRef: geometryRef, cameraWindowRef: windowRef,
    alignBestRef: { current: { best: null, box: null } }, alignImageMs: { current: 0 }, setAlignImage: () => undefined, camStatus: "running",
    reviewCanvasRef: { current: null }, reviewPath: "", roiOverlayRef: overlayRef, waferGridOverlayRef: { current: null },
  } as never);
  return null;
}

const frame = (width = 816, height = 624) => ({ valid: true, width, height, stride_bytes: width, byte_len: width * height, frame_index: "1", data: new Uint8Array(4) });
const draw = (packet = frame()) => act(async () => { deliver(packet); });

beforeEach(async () => {
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
  ctx = { putImageData: vi.fn(), strokeRect: vi.fn(), strokeStyle: "", lineWidth: 0 };
  canvas = { width: 0, height: 0, getContext: () => ctx };
  geometryRef = { current: geometry() };
  windowRef = { current: { x: 100, y: 120, width: 256, height: 128 } };
  overlayRef = { current: true };
  await act(async () => { root.render(<Harness />); });
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });

describe("the camera-window box on the live frame", () => {
  it("is drawn in yellow at the stored window while the ROI overlay is on", async () => {
    await draw();
    expect(ctx.strokeStyle).toBe("#ffd400");
    expect(ctx.strokeRect).toHaveBeenCalledTimes(1);
    expect(ctx.strokeRect).toHaveBeenCalledWith(101, 121, 254, 126);
  });

  it("is not drawn while the overlay is off, and comes back with it", async () => {
    overlayRef.current = false;
    await draw();
    expect(ctx.putImageData).toHaveBeenCalledTimes(1);   // the frame itself still is
    expect(ctx.strokeRect).not.toHaveBeenCalled();
    overlayRef.current = true;
    await draw();
    expect(ctx.strokeRect).toHaveBeenCalledTimes(1);
  });

  it("is mirrored across the frame for a profile that flips it, from the stored sensor coordinates", async () => {
    geometryRef.current = geometry({ flip_horizontal: true });
    await draw();
    expect(ctx.strokeRect).toHaveBeenLastCalledWith(461, 121, 254, 126);            // x: 816 - 256 - 100 = 460
    geometryRef.current = geometry({ flip_horizontal: true, flip_vertical: true });
    await draw();
    expect(ctx.strokeRect).toHaveBeenLastCalledWith(461, 377, 254, 126);            // y: 624 - 128 - 120 = 376
    expect(windowRef.current).toEqual({ x: 100, y: 120, width: 256, height: 128 }); // the stored window is untouched
  });

  it("is only drawn in the Overview, on a frame of the sensor's size (an eGrabber Overview frame may differ)", async () => {
    geometryRef.current = geometry({ overview: false });
    await draw();
    geometryRef.current = geometry();
    await draw(frame(640, 480));
    expect(ctx.strokeRect).not.toHaveBeenCalled();
    geometryRef.current = geometry({ camera: "egrabber", sensor_width: 1920, sensor_height: 1080 });
    await draw(frame(1920, 100));
    expect(ctx.strokeRect).toHaveBeenCalledTimes(1);
  });
});
