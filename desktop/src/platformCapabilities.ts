// #501: which surfaces this instrument has. The backend reports them in
// fetch_platform_info `capabilities`; a server without the report (older
// builds) is the MIB desktop, so every MIB surface stays.
//
// Pure module: unit-testable in plain Node.

import type { PlatformCapabilities, PlatformInfo } from "./bridge";

export const DESKTOP_CAPABILITIES: Readonly<PlatformCapabilities> = {
  instrument: "desktop",
  autofocus: true,
  trigger: true,
  host_background: true,
  frame_buffer: true,
  reanalysis: true,
  core_updates: true,
  egrabber_script: true,
  pl_identity: false,
  led_strobe: false,
  align_mode: false,
  run_mode: false,
  pump: null,
};

export function capabilitiesOf(platform: PlatformInfo | null): PlatformCapabilities {
  if (!platform?.capabilities) return { ...DESKTOP_CAPABILITIES };
  return { ...DESKTOP_CAPABILITIES, ...platform.capabilities };
}

export const isPz7035 = (c: PlatformCapabilities) => c.instrument === "pz7035";

/** The processing settings the PL's unet_cells_v2 does not implement (#651 G7). Mirrors
 *  plIgnoredSettingsChanged in PzProfileCompiler.cpp, which names the ones the operator changed. */
export const PL_IGNORED_SETTINGS_TEXT =
  "gaussian blur, background-subtract threshold, morphology, the border check, the ring-ratio gate, " +
  "the single-inner-contour requirement, automatic background or ROI, and the empty-frame threshold";
