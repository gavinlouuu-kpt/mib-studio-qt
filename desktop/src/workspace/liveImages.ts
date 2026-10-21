// A live cell's crop or mask packet (Mono8, pull kinds 5 and 6) as an image URL for the kit's
// CellImage (#716 A6b). The kit takes an image URL, so the packet goes through a canvas; a canvas or
// ImageBitmap input is requested on #715 (as for Review's crops).
//
// The crop is drawn as recorded. The mask is drawn in the kit's accent colour where the recorded mask
// is set and transparent elsewhere, so the kit's mask overlay (multiplied at 45 %) tints the object.
import { mono8ToImageData } from "../bridge";
import type { FramePacket } from "../framePacket";
import type { CellLayer } from "./useLiveCells";

/** --mib-color-accent (#2f6e6a). */
const MASK_RGB = [0x2f, 0x6e, 0x6a] as const;

export function maskRgba(p: Pick<FramePacket, "data" | "width" | "height" | "stride_bytes">): Uint8ClampedArray<ArrayBuffer> {
  const rgba = new Uint8ClampedArray(p.width * p.height * 4);
  const stride = p.stride_bytes > 0 ? p.stride_bytes : p.width;
  for (let y = 0; y < p.height; y++) {
    for (let x = 0; x < p.width; x++) {
      if ((p.data[y * stride + x] ?? 0) === 0) continue;
      const d = (y * p.width + x) * 4;
      rgba[d] = MASK_RGB[0];
      rgba[d + 1] = MASK_RGB[1];
      rgba[d + 2] = MASK_RGB[2];
      rgba[d + 3] = 255;
    }
  }
  return rgba;
}

/** The packet as a PNG data URL; null when it cannot be drawn (an empty packet, no 2D canvas). */
export function packetToUrl(p: FramePacket, layer: CellLayer, doc: Document = document): string | null {
  if (!p.valid || p.width <= 0 || p.height <= 0) return null;
  const canvas = doc.createElement("canvas");
  canvas.width = p.width;
  canvas.height = p.height;
  const g = canvas.getContext("2d");
  if (!g) return null;
  g.putImageData(layer === "image" ? mono8ToImageData(p.data, p.width, p.height, p.stride_bytes) : new ImageData(maskRgba(p), p.width, p.height), 0, 0);
  try {
    return canvas.toDataURL("image/png");
  } catch {
    return null;
  }
}
