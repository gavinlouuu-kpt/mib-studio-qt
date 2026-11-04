// The review charts' colours from the @mib/ui tokens (#717 B3a: the app's
// canvas scatter restyled with the kit, coordinator ruling on #717). Canvas
// 2D cannot read CSS variables in an offscreen export, so the token values
// are mirrored here (desktop/ui/src/styles/tokens.css); the on-screen chart
// and the exported PNGs draw with the same palette, as before. Charts are
// compared with Qt through their data, not their pixels
// (tools/review_parity).
export const CHART_PALETTE = {
  /** --mib-color-card */
  background: "#ffffff",
  /** --mib-color-line */
  grid: "#e1e6e6",
  /** --mib-color-mute */
  frame: "#8c9797",
  /** --mib-color-sub */
  tick: "#5f6b6b",
  /** --mib-color-ink */
  ink: "#1b2626",
  /** --mib-color-accent: every valid record */
  point: "#2f6e6a",
  /** --mib-color-inspect: the inspected record */
  inspect: "#3a6ea5",
  /** --mib-color-mute */
  empty: "#8c9797",
  font: "Inter, system-ui, sans-serif",
} as const;
