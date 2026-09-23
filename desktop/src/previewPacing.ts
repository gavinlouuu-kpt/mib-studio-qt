// Keep packet admission/backpressure separate from display pacing. Profile
// display_fps changes how often the shell asks for a frame, not camera timing.
export function previewIntervalMs(fps:unknown):number {
 const value=typeof fps==="number"&&Number.isFinite(fps)?Math.min(240,Math.max(1,fps)):30;
 return 1000/value;
}
