// A run saved by a PL-science instrument (PZ7035) holds one metadata row per cell and no image or mask
// pixels (#649 parity spec, section 6). Review opens it, but its frame, thumbnail and mask views are driven
// by the image datasets and would just read 0 frames: say why instead (#649 decision 7).

interface FileCounts {
  file_open?: boolean;
  recording_file?: boolean;
  // The standalone Review sends counts as decimal strings, the desktop bridge as numbers.
  total_valid?: number | string;
  total_invalid?: number | string;
  valid_images?: { count: number | string };
  invalid_images?: { count: number | string };
  recorded_images?: { count: number | string };
}

export function noImagesNotice(meta: FileCounts | null | undefined): string | null {
  if (!meta?.file_open || meta.recording_file) return null;
  const rows = Number(meta.total_valid ?? 0) + Number(meta.total_invalid ?? 0);
  const images = Number(meta.valid_images?.count ?? 0) + Number(meta.invalid_images?.count ?? 0) + Number(meta.recorded_images?.count ?? 0);
  if (images > 0 || rows <= 0) return null;
  return `This file has no images or masks: a run on a PL-science instrument saves one metadata row per cell and no pixels. ` +
    `The metrics table, charts and export work; the frame, thumbnail and mask views have nothing to show.`;
}
