// A record's crop as an image URL for the kit's CellImage (#717 B3a): the
// backend composes the overlay into the frame packet (`fetch_review_frame`),
// the client cuts the square around the record's bounding box. The kit takes
// an image URL, so the crop goes through a canvas (a canvas / ImageBitmap
// input is requested on #715).
import type { FramePacket } from "../../framePacket";
import { packetToImageData } from "../reviewBridge";
import type { CropRect } from "./inspectModel";

/** The crop as a PNG data URL, or null when it cannot be drawn (no 2D
 * canvas, an empty packet, or a rectangle outside the packet). */
export function cropPacketToDataUrl(p: FramePacket, rect: CropRect, doc: Document = document): string | null {
  if (!p.valid || p.width <= 0 || p.height <= 0) return null;
  const size = Math.min(rect.size, p.width, p.height);
  const x = Math.min(Math.max(0, rect.x), p.width - size);
  const y = Math.min(Math.max(0, rect.y), p.height - size);
  const canvas = doc.createElement("canvas");
  canvas.width = size;
  canvas.height = size;
  const g = canvas.getContext("2d");
  if (!g) return null;
  // The dirty rectangle (x, y, size, size) of the frame lands at (0, 0).
  g.putImageData(packetToImageData(p), -x, -y, x, y, size, size);
  try {
    return canvas.toDataURL("image/png");
  } catch {
    return null;
  }
}
