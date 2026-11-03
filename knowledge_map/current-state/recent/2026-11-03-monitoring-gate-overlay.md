## 2026-11-03 — Target-group gate drawn and draggable on the Monitoring scatter

The spin-box numbers of the tune panel's target-group gate were hard to
translate into the Deformability-vs-Area scatter. [[ExperimentMonitoringTab]]
now draws the **draft** gate on the scatter (only while `TargetGroupEnabled` is
on in the draft): a tinted "would be target" region, the dashed
area/deformability box and, with the E gate on, the dash-dot iso-E contours with
value labels ("E ≥ 1.00 kPa"), named "Target group" or "Target group
(unapplied)". Pressing within 6 px of a box edge or an E contour and dragging
writes the tune panel's own spin box, so the draft, the `*` markers and the
footer follow and Apply is still required; clamped to the control's range,
rounded to its precision, min ≤ max, and not while an apply is in flight or the
control is disabled. A drag that starts elsewhere still pans, a click without
travel still selects a cell. Empty plot shows an "Area … · deformability … · E …
kPa" tooltip; a label under the chart gives the live "Target group preview: N of
M plotted valid cells (P%)". With the E gate on and no table loaded the E layer
is skipped: the chart tints and counts the box and says "No E-modulus table
loaded: the target group uses the box only" (first version: "the E range admits
no cell", replaced by [[2026-11-03-emodulus-gate-needs-lut]]). HDF5 snapshots
and TIFF exports hide the overlay like the selection highlight.

Design: the predicate, mask, iso-E contours (marching squares on the
`EModulusLut` grid), hit testing, drag maths and count are a pure Qt-Core model,
`frontend::gate` (`TargetGatePreview.h`, [[System-Utilities]]); the overlay is a
chart child item that reads the live axes at paint time. `ZoomableChartView`
gained an optional press interceptor with `interceptDrag*` signals (and
`hoverLeft`); without an interceptor nothing changes. The tab reads the
E-modulus table from a private copy taken on the GUI thread (refreshed when the
service's locked table identity changes), never from the live object.

Guards: `frontend.target_gate_preview` (pure) and
`frontend.monitoring_gate_overlay` (widget, incl. the view's interceptor).
Worth knowing (fixed the same day, [[2026-11-03-emodulus-gate-needs-lut]]): with
no table loaded `ProcessingScience.cpp` leaves `youngsModulus` at 0.0 (not NaN),
so an enabled E range containing 0 admitted cells in the backend while the
overlay and the count said none, and a positive minimum admitted none. The rule
is now: no table, no E layer.
