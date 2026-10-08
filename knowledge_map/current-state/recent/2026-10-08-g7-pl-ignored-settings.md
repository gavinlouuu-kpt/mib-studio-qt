## 2026-10-08 — Processing settings the PL does not implement are reported (#651 G7)

The PL's `unet_cells_v2` implements the area, deformability, area-ratio and Laplacian gates, the target group
and its E-modulus gate, the channel band, the minimum cell area and the store flags (including multi-image), but
not the host mask pipeline (blur, background subtraction, morphology), the border check, the ring-ratio gate, the
single-inner-contour requirement, automatic background/ROI or the empty-frame threshold; those used to be accepted
and silently ignored. `plIgnoredSettingsChanged` (`PzProfileCompiler.cpp`) names the ones that differ from their
defaults; `CompiledProfile::warnings` carries them (the profile still compiles), the `processing.profileCompile`
readiness gate becomes a non-blocking Warn listing them, and the PL-science config panel says which settings have no
effect. See [[Desktop-Shell]] and [[PZ7035-Records]].
