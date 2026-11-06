// Frame viewer "Export as TIFF…" naming (#867 REVIEW-031), the Qt
// FrameViewerDialog::onExportFrame rules: the save dialog proposes
// `frame_<frame index>.tiff`, or `frame_<frame index>_series_<k>.tiff` when the
// frame has a series (k is the 0-based series image shown; 0 is the trigger
// image), and `.tiff` is appended unless the name already ends .tiff / .tif.
// Pure; unit-tested in review.test.ts.

export function frameTiffName(frameIndex: string | number, series: { position: number } | null): string {
  return series ? `frame_${frameIndex}_series_${series.position}.tiff` : `frame_${frameIndex}.tiff`;
}

export function withTiffExtension(path: string): string {
  return /\.(tiff|tif)$/i.test(path) ? path : `${path}.tiff`;
}
