// Target-group sorting gate, previewed and edited on the Monitoring scatter.
//
// Pure model (QtCore + the Qt-free backend types only, no widgets) behind the
// gate overlay of the Deformability-vs-Area scatter in ExperimentMonitoringTab:
//
//  - GateSpec is the draft gate (TargetGroupEnabled, target area / deformability
//    box, optional Young's modulus range) and isTarget() is the exact predicate
//    of ProcessingScience.cpp (isTargetGroupMember): a valid cell is target group
//    when its area and deformability lie in the box and the E layer passes. The E
//    layer filters only with the E gate on AND an E-modulus table loaded; then the
//    cell's E (EModulusLut::lookup(area, deformability)) must be a number inside
//    [emodulus_min, emodulus_max]. With no table loaded the E layer is skipped and
//    the group is the box alone (a two-layer filter), whatever the E range says.
//  - computeMask() samples that predicate over the plot area (tinted region).
//  - isoContour() extracts the iso-E lines of the LUT (marching squares on the
//    LUT's own bilinear grid; the crossings sit on grid edges, where lookup()
//    is exactly linear, so lookup(point) == level up to rounding).
//  - hitTest() / beginDrag() / dragValue() / constrainEdit() turn pointer
//    positions into edits of one gate field: nearest box edge or E contour
//    within a pixel tolerance, relative drag (no jump on press), clamp to the
//    control's range, round to the field's display precision, keep min <= max.
//  - countTargets() / previewText() give the live "N of M" count on the plotted
//    cells.
//
// All pixel coordinates are in one system (the chart item's, where the plot
// rectangle lives); scatterhit::Viewport carries the axis ranges and that
// rectangle. Data coordinates are (area um^2, deformability).
#pragma once

#include "backend/processing/EModulusLut.h"
#include "backend/processing/ProcessingTypes.h"
#include "frontend/models/ProcessingConfigDraft.h"
#include "frontend/utils/ScatterHitTest.h"

#include <QPointF>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace frontend::gate {

using Viewport = scatterhit::Viewport;
using Polyline = std::vector<QPointF>; // data space: x = area (um^2), y = deformability
using Contour = std::vector<Polyline>; // one iso-E level: open lines and closed loops

// ---- the gate -----------------------------------------------------------------

struct GateSpec {
    bool enabled{false};
    double areaMin{0.0}, areaMax{0.0};
    double deformMin{0.0}, deformMax{0.0};
    bool emodEnabled{false};
    double emodMin{0.0}, emodMax{0.0};

    static GateSpec fromConfig(const backend::services::ProcessingConfig& cfg);
    bool operator==(const GateSpec& o) const;
    bool operator!=(const GateSpec& o) const { return !(*this == o); }
};

// E (kPa) for a point, NaN without a loaded table or outside its coverage.
double lookupE(const backend::EModulusLut* lut, double areaUm, double deformability);

// Whether a table is usable for the gate: non-null and loaded.
bool lutLoaded(const backend::EModulusLut* lut);

// Whether the E layer filters at all: the gate has an E range AND a table is loaded. Otherwise the
// target group is the box alone (the contours and the E part of the tint are not drawn either).
bool eLayerFilters(const GateSpec& spec, bool tableLoaded);

// ProcessingScience semantics for a *valid* cell. `emodulus` is the cell's E (NaN = no value, i.e.
// outside the table); it is not read unless the E layer filters (eLayerFilters). With the layer
// filtering, a NaN E fails it. False whenever the gate is off.
bool isTarget(const GateSpec& spec, bool tableLoaded, double areaUm, double deformability, double emodulus);
// Same with E looked up in `lut` (a null or unloaded table skips the E layer).
bool wouldBeTarget(const GateSpec& spec, const backend::EModulusLut* lut, double areaUm, double deformability);

// ---- data <-> pixel -------------------------------------------------------------

bool viewportValid(const Viewport& v);
QPointF toPixel(const Viewport& v, const QPointF& data);
QPointF toData(const Viewport& v, const QPointF& pixel);

// ---- tinted region ----------------------------------------------------------------

// Predicate sampled at the centre of every step x step pixel cell of the plot
// rectangle (row-major from the top-left; cells at the right / bottom edge may
// overhang the plot).
struct Mask {
    int cols{0}, rows{0};
    double stepPx{4.0};
    std::vector<std::uint8_t> on;
    std::size_t onCount() const;
    std::size_t cellCount() const { return on.size(); }
    bool at(int col, int row) const;
};
Mask computeMask(const GateSpec& spec, const backend::EModulusLut* lut, const Viewport& viewport, double stepPx);

// ---- iso-E contours ---------------------------------------------------------------

// The `level` (kPa) contour of the table, in data space. Empty when the table is
// not loaded or the level lies outside its values.
Contour isoContour(const backend::EModulusLut& lut, double level);

// ---- handles ------------------------------------------------------------------------

enum class HandleKind { None, AreaMin, AreaMax, DeformMin, DeformMax, EmodMin, EmodMax };

constexpr double kDefaultHandleTolerancePx = 6.0;

TuneField handleField(HandleKind kind); // the tune field a handle edits; kind != None
bool isVerticalEdge(HandleKind kind);   // area edge: a vertical line, dragged horizontally
bool isHorizontalEdge(HandleKind kind); // deformability edge
bool isEmodHandle(HandleKind kind);
double handleValue(const GateSpec& spec, HandleKind kind);

// Contours of the two E levels (null = not drawn / no table).
struct EContours {
    const Contour* min{nullptr};
    const Contour* max{nullptr};
};

struct HandleHit {
    HandleKind kind{HandleKind::None};
    double distancePx{std::numeric_limits<double>::infinity()};
    QPointF anchorPx; // nearest point on the handle (pixels)
    explicit operator bool() const { return kind != HandleKind::None; }
};

// The handle nearest to (px, py) within tolerancePx; nothing unless the gate is
// on and the pointer is inside the plot rectangle. Edges win ties against E
// contours; of two coincident edges the one on the pointer's side is taken.
HandleHit hitTest(const GateSpec& spec, const EContours& contours, const Viewport& viewport, double px, double py,
                  double tolerancePx = kDefaultHandleTolerancePx);

// A representative pixel position on a handle, inside the plot (middle of the
// visible edge, or a middle vertex of the E contour); nullopt when not visible.
std::optional<QPointF> handleAnchor(HandleKind kind, const GateSpec& spec, const EContours& contours,
                                    const Viewport& viewport);

// Where to write the value label of an E contour: its right-most vertex inside
// the plot (nullopt when no vertex is visible).
std::optional<QPointF> contourLabelAnchor(const Contour& contour, const Viewport& viewport);

// ---- editing ------------------------------------------------------------------------------

struct Limits {
    double lo{0.0};
    double hi{0.0};
};
// The tune panel's spin box ranges (area 0..100000, deformability 0..1, E 0..1000).
Limits defaultLimits(TuneField field);

// Round to the field's display precision (tuneFieldDecimals).
double roundToField(TuneField field, double value);

// Clamp to `limits`, keep min <= max against the *other* bound of `current`,
// round to the field's precision.
double constrainEdit(HandleKind kind, double raw, const GateSpec& current, const Limits& limits);

struct DragSession {
    HandleKind kind{HandleKind::None};
    double valueAtPress{0.0};
    QPointF pressPx;
    QPointF anchorOffsetPx; // E handles: nearest contour point minus the press
    double eAtAnchor{std::numeric_limits<double>::quiet_NaN()};
};
DragSession beginDrag(const HandleHit& hit, const GateSpec& spec, const backend::EModulusLut* lut,
                      const Viewport& viewport, const QPointF& pressPx);

// New value for the dragged field with the pointer at nowPx, already constrained.
// Relative to the press (no jump): an edge moves by the pointer's travel; an E
// contour by the change of E under the contour's anchor point carried along with
// the pointer. nullopt when an E lookup has no value there (keep the last value).
std::optional<double> dragValue(const DragSession& session, const GateSpec& current,
                                const backend::EModulusLut* lut, const Viewport& viewport, const QPointF& nowPx,
                                const Limits& limits);

// ---- live count -------------------------------------------------------------------------------

struct PreviewCount {
    int target{0};
    int total{0};
    double percent() const { return total > 0 ? 100.0 * target / total : 0.0; }
};
// `points` are the plotted valid cells (x = um^2, y = deformability), `emodulus`
// their E (NaN = none), same order; `tableLoaded` is whether the E-modulus table is
// loaded (when it is not, the E values are not read: the E layer is skipped).
// Counts the cells isTarget() accepts.
PreviewCount countTargets(const GateSpec& spec, bool tableLoaded, const std::vector<scatterhit::Point>& points,
                          const std::vector<double>& emodulus);
QString previewText(const PreviewCount& count);

} // namespace frontend::gate
