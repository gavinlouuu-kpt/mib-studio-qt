// target_gate_preview_test
//
// Pure model behind the gate overlay of the Monitoring scatter
// (frontend/models/TargetGatePreview.h), no widgets:
//  - gate::isTarget / wouldBeTarget equal the ProcessingScience.cpp target
//    group expression (tgArea && tgDeform && tgEmod, inclusive bounds, NaN E
//    never passes an enabled E gate, E gate off ignores E, gate off is never
//    target) on a dense sample that crosses every bound and the table's edge;
//  - the sampled mask is that predicate evaluated at the cell centres;
//  - iso-E contours: every vertex satisfies |lookup - level| ~ 0, both saddle
//    resolutions of a bilinear cell, a closed loop, nothing outside the table;
//  - hit testing picks the nearest box edge or E contour within the tolerance,
//    edges end where the box ends, coincident edges split by pointer side;
//  - dragging is relative to the press (no jump), clamps to the control range,
//    rounds to the display precision, keeps min <= max, ignores positions
//    without an E value;
//  - the live count follows the same predicate (NaN E fails an enabled E gate).

#include "frontend/models/TargetGatePreview.h"

#include "support/assert.h"

#include <QCoreApplication>

#include <cmath>
#include <limits>

using backend::EModulusLut;
using backend::services::ProcessingConfig;
using namespace frontend;
namespace g = frontend::gate;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// E(area, deformability) = 0.5 + area / 100 * (1 - 2 * deformability): exactly
// representable by the bilinear interpolation, so lookups are checkable by formula.
double synthE(double area, double deform)
{
    return 0.5 + area / 100.0 * (1.0 - 2.0 * deform);
}

// 61 x 51 grid over area 0..600 um^2, deformability 0..0.5; the top-right
// corner block is NaN ("no value") to exercise missing coverage.
EModulusLut makeLut()
{
    const std::size_t na = 61, nd = 51;
    std::vector<double> grid(na * nd);
    for (std::size_t di = 0; di < nd; ++di)
        for (std::size_t ai = 0; ai < na; ++ai) {
            const double a = 10.0 * static_cast<double>(ai);
            const double d = 0.01 * static_cast<double>(di);
            grid[di * na + ai] = (ai >= 55 && di >= 45) ? kNaN : synthE(a, d);
        }
    EModulusLut lut;
    MIB_REQUIRE(lut.loadGrid(0.0, 10.0, na, 0.0, 0.01, nd, std::move(grid)), "synthetic table loads");
    return lut;
}

// The expression of ProcessingScience.cpp (isTargetGroupMember), spelled out
// from a ProcessingConfig for a valid cell with E already known. The E layer
// filters only with the E gate on AND a table loaded; without a table the E is
// not read and the group is the box alone.
bool scienceTarget(const ProcessingConfig& c, bool tableLoaded, double areaUm, double deform, double e)
{
    if (!c.enable_target_group) return false;
    const bool tgArea = (areaUm >= c.target_group_area_min && areaUm <= c.target_group_area_max);
    const bool tgDeform = (deform >= c.target_group_deformability_min && deform <= c.target_group_deformability_max);
    const bool tgEmod = !c.enable_target_group_emodulus || !tableLoaded ||
                        (!std::isnan(e) && e >= c.target_group_emodulus_min && e <= c.target_group_emodulus_max);
    return tgArea && tgDeform && tgEmod;
}

g::Viewport makeViewport()
{
    g::Viewport v;
    v.x0 = 0.0;
    v.x1 = 600.0;
    v.y0 = 0.0;
    v.y1 = 0.5;
    v.left = 50.0;
    v.top = 20.0;
    v.width = 600.0;
    v.height = 400.0; // 1 px per um^2, 800 px per deformability unit
    return v;
}

g::GateSpec boxSpec(bool emod = false, double eMin = 1.0, double eMax = 3.0)
{
    g::GateSpec s;
    s.enabled = true;
    s.areaMin = 100;
    s.areaMax = 300;
    s.deformMin = 0.1;
    s.deformMax = 0.3;
    s.emodEnabled = emod;
    s.emodMin = eMin;
    s.emodMax = eMax;
    return s;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    const EModulusLut lut = makeLut();
    const g::Viewport vp = makeViewport();

    // ---- 1. the predicate is the science expression -----------------------------
    {
        MIB_EXPECT(std::isnan(g::lookupE(nullptr, 100, 0.1)), "no table: no E");
        EModulusLut empty;
        MIB_EXPECT(std::isnan(g::lookupE(&empty, 100, 0.1)), "unloaded table: no E");
        MIB_EXPECT(std::abs(g::lookupE(&lut, 123.4, 0.137) - synthE(123.4, 0.137)) < 1e-9, "lookup helper = table lookup");

        MIB_EXPECT(g::lutLoaded(&lut) && !g::lutLoaded(nullptr) && !g::lutLoaded(&empty), "lutLoaded: loaded only");
        MIB_EXPECT(g::eLayerFilters(boxSpec(true), true) && !g::eLayerFilters(boxSpec(true), false) &&
                       !g::eLayerFilters(boxSpec(false), true),
                   "the E layer filters with the E gate on and a table loaded");

        long compared = 0, targets = 0, nanE = 0;
        for (int variant = 0; variant < 8; ++variant) {
            ProcessingConfig c;
            c.enable_target_group = variant != 0;
            c.target_group_area_min = 100;
            c.target_group_area_max = 300;
            c.target_group_deformability_min = 0.1;
            c.target_group_deformability_max = 0.3;
            c.enable_target_group_emodulus = variant >= 3;
            // 3: wide, 4: narrow band, 5: band that only the NaN-free part of the table can hold,
            // 6: a positive minimum no placeholder E (0.0) satisfies, 7: a band above the table's values
            c.target_group_emodulus_min = variant == 4 ? 1.5 : (variant == 6 ? 5.0 : (variant == 7 ? 50.0 : 0.0));
            c.target_group_emodulus_max = variant == 4 ? 2.5 : (variant == 5 ? 4.0 : (variant == 7 ? 60.0 : 10.0));
            const g::GateSpec spec = g::GateSpec::fromConfig(c);
            MIB_EXPECT(spec.enabled == c.enable_target_group && spec.emodEnabled == c.enable_target_group_emodulus,
                       "spec mirrors the config flags");
            // Dense sweep over area 0..700 (past the table's 600), deformability 0..0.55 (past 0.5),
            // landing exactly on the bounds (100, 300, 0.1, 0.3 are sample points).
            for (int ia = 0; ia <= 140; ++ia) {
                for (int id = 0; id <= 55; ++id) {
                    const double a = 5.0 * ia;
                    const double d = 0.01 * id;
                    const double e = g::lookupE(&lut, a, d);
                    if (std::isnan(e)) ++nanE;
                    const bool want = scienceTarget(c, true, a, d, e);
                    MIB_EXPECT(g::wouldBeTarget(spec, &lut, a, d) == want, "wouldBeTarget == science");
                    MIB_EXPECT(g::isTarget(spec, true, a, d, e) == want, "isTarget == science");
                    // No table: the E layer is skipped whatever E the cell carries (0.0 placeholder, NaN, or a value).
                    const bool wantBox = scienceTarget(c, false, a, d, e);
                    MIB_EXPECT(g::wouldBeTarget(spec, nullptr, a, d) == wantBox, "wouldBeTarget, no table == science");
                    MIB_EXPECT(g::isTarget(spec, false, a, d, 0.0) == wantBox && g::isTarget(spec, false, a, d, kNaN) == wantBox &&
                                   g::isTarget(spec, false, a, d, e) == wantBox,
                               "isTarget, no table == science (the E is not read)");
                    MIB_EXPECT(wantBox == (c.enable_target_group && a >= 100 && a <= 300 && d >= 0.1 && d <= 0.3),
                               "no table: the box alone");
                    ++compared;
                    targets += want;
                }
            }
        }
        MIB_EXPECT(compared == 8 * 141 * 56 && targets > 100 && nanE > 1000, "the sweep covers targets and missing E");
        const g::GateSpec on = boxSpec(true, 1.0, 3.0);
        MIB_EXPECT(!g::isTarget(on, true, 200, 0.2, kNaN), "NaN E fails an enabled E gate (table loaded)");
        MIB_EXPECT(g::isTarget(on, false, 200, 0.2, kNaN) && g::isTarget(on, false, 200, 0.2, 0.0) &&
                       g::isTarget(boxSpec(true, 5.0, 10.0), false, 200, 0.2, 0.0),
                   "no table: an enabled E gate is skipped (min 1 or 5, E 0.0 or NaN)");
        MIB_EXPECT(!g::isTarget(on, false, 50, 0.2, 2.0) && !g::isTarget(on, false, 200, 0.5, 2.0),
                   "no table: a cell outside the box is out");
        MIB_EXPECT(g::isTarget(on, true, 200, 0.2, 2.0) && !g::isTarget(on, true, 200, 0.2, 0.0) && !g::isTarget(on, true, 200, 0.2, 3.5),
                   "table loaded: the range filters");
        MIB_EXPECT(g::isTarget(boxSpec(false), true, 200, 0.2, kNaN), "NaN E is ignored with the E gate off");
        MIB_EXPECT(!g::isTarget(boxSpec(false), true, kNaN, 0.2, 2.0) && !g::isTarget(boxSpec(false), true, 200, kNaN, 2.0),
                   "a NaN area or deformability is never in range");
        g::GateSpec off = boxSpec(false);
        off.enabled = false;
        MIB_EXPECT(!g::isTarget(off, true, 200, 0.2, 2.0) && !g::isTarget(off, false, 200, 0.2, 2.0), "gate off: never target");
        MIB_EXPECT(g::isTarget(boxSpec(false), true, 100, 0.1, 0) && g::isTarget(boxSpec(false), true, 300, 0.3, 0), "bounds are inclusive");
        MIB_EXPECT(!g::isTarget(boxSpec(false), true, 99.999, 0.2, 0) && !g::isTarget(boxSpec(false), true, 300.001, 0.2, 0), "just outside is out");
        MIB_EXPECT(g::GateSpec{} == g::GateSpec{} && boxSpec() != boxSpec(true), "spec equality");
    }

    // ---- 2. mask = predicate at the cell centres --------------------------------------
    {
        for (bool emod : {false, true}) {
            const g::GateSpec spec = boxSpec(emod, 1.0, 3.0);
            const g::Mask m = g::computeMask(spec, &lut, vp, 3.0);
            MIB_REQUIRE(m.cols == 200 && m.rows == 134 && m.cellCount() == 200u * 134u, "grid of 3 px cells over the plot");
            bool allMatch = true;
            for (int r = 0; r < m.rows; ++r)
                for (int c = 0; c < m.cols; ++c) {
                    const double px = vp.left + (c + 0.5) * m.stepPx;
                    const double py = vp.top + (r + 0.5) * m.stepPx;
                    const bool inside = px <= vp.left + vp.width && py <= vp.top + vp.height;
                    const QPointF d = g::toData(vp, QPointF(px, py));
                    const bool want = inside && g::wouldBeTarget(spec, &lut, d.x(), d.y());
                    if (m.at(c, r) != want) allMatch = false;
                }
            MIB_EXPECT(allMatch, emod ? "mask == predicate (E gate on)" : "mask == predicate (E gate off)");
            if (!emod) {
                // 200 x 0.2 um^2 x deformability units = 200 px x 160 px
                const double expected = 200.0 * 160.0 / (3.0 * 3.0);
                MIB_EXPECT(std::abs(static_cast<double>(m.onCount()) - expected) < 0.03 * expected, "box area as tinted cells");
            } else {
                const g::Mask plain = g::computeMask(boxSpec(false), &lut, vp, 3.0);
                MIB_EXPECT(m.onCount() > 0 && m.onCount() < plain.onCount(), "an E range removes cells from the box");
            }
        }
        g::GateSpec off = boxSpec(true);
        off.enabled = false;
        MIB_EXPECT(g::computeMask(off, &lut, vp, 3.0).onCount() == 0, "gate off: empty mask");
        {
            // No table: the E layer is skipped, the tint is the box whatever the E range says.
            const g::Mask plainBox = g::computeMask(boxSpec(false), nullptr, vp, 3.0);
            MIB_EXPECT(plainBox.onCount() > 0, "E gate off needs no table");
            for (const double eMin : {0.0, 5.0}) {
                const g::Mask noTable = g::computeMask(boxSpec(true, eMin, 10.0), nullptr, vp, 3.0);
                MIB_EXPECT(noTable.onCount() == plainBox.onCount() && noTable.on == plainBox.on,
                           "E gate on, no table: the mask is the box (E min " + std::to_string(eMin) + ")");
                const backend::EModulusLut unloaded;
                MIB_EXPECT(g::computeMask(boxSpec(true, eMin, 10.0), &unloaded, vp, 3.0).on == plainBox.on,
                           "E gate on, unloaded table: the mask is the box (E min " + std::to_string(eMin) + ")");
            }
            // With a table the E range still removes cells from the box.
            MIB_EXPECT(g::computeMask(boxSpec(true, 1.0, 3.0), &lut, vp, 3.0).onCount() < plainBox.onCount(),
                       "E gate on, table: the range removes cells from the box");
        }
        g::Viewport bad = vp;
        bad.x1 = bad.x0;
        MIB_EXPECT(g::computeMask(boxSpec(), &lut, bad, 3.0).cellCount() == 0, "degenerate viewport: empty");
    }

    // ---- 3. iso-E contours ----------------------------------------------------------------
    {
        for (double level : {0.7, 1.0, 2.5, 4.0, 5.5}) {
            const g::Contour contour = g::isoContour(lut, level);
            MIB_EXPECT(!contour.empty(), "level inside the table has a contour");
            std::size_t vertices = 0;
            double worst = 0.0;
            for (const auto& line : contour) {
                MIB_EXPECT(line.size() >= 2, "a contour line has at least two vertices");
                for (const QPointF& v : line) {
                    const double e = lut.lookup(v.x(), v.y());
                    // lookup() reads the cell above / right of a point on a grid line, so a vertex on the
                    // border of the table's missing-value block (area >= 550, deformability >= 0.45) has
                    // no value there; every other vertex must.
                    const bool onMissingBlock = v.x() >= 540.0 && v.y() >= 0.44;
                    if (std::isnan(e)) {
                        MIB_EXPECT(onMissingBlock, "vertex outside the missing-value block has an E");
                        continue;
                    }
                    worst = std::max(worst, std::abs(e - level));
                    ++vertices;
                }
            }
            MIB_EXPECT(vertices >= 4 && worst < 1e-9, "every vertex satisfies |lookup - level| ~ 0 (worst " + std::to_string(worst) + ")");
        }
        MIB_EXPECT(g::isoContour(lut, 100.0).empty() && g::isoContour(lut, -1.0).empty(), "levels outside the table: no contour");
        MIB_EXPECT(g::isoContour(lut, kNaN).empty(), "NaN level: no contour");
        EModulusLut empty;
        MIB_EXPECT(g::isoContour(empty, 1.0).empty(), "unloaded table: no contour");

        // Both resolutions of a saddle cell (bilinear centre decides).
        EModulusLut saddle;
        MIB_REQUIRE(saddle.loadGrid(0, 1, 2, 0, 1, 2, {0.0, 1.0, 1.0, 0.0}), "2 x 2 saddle table");
        for (double level : {0.4, 0.6}) {
            const g::Contour c = g::isoContour(saddle, level);
            MIB_EXPECT(c.size() == 2, "a saddle cell has two separate lines");
            for (const auto& line : c) {
                MIB_EXPECT(line.size() == 2, "each is one segment");
                for (const QPointF& v : line) MIB_EXPECT(std::abs(saddle.lookup(v.x(), v.y()) - level) < 1e-12, "saddle vertex on level");
            }
        }
        // A closed loop around a peak.
        EModulusLut peak;
        std::vector<double> grid;
        for (int di = 0; di < 7; ++di)
            for (int ai = 0; ai < 7; ++ai) grid.push_back(10.0 - ((ai - 3) * (ai - 3) + (di - 3) * (di - 3)));
        MIB_REQUIRE(peak.loadGrid(0, 1, 7, 0, 1, 7, std::move(grid)), "peak table");
        const g::Contour loop = g::isoContour(peak, 5.5);
        MIB_REQUIRE(loop.size() == 1 && loop[0].size() >= 5, "one closed loop");
        MIB_EXPECT(loop[0].front() == loop[0].back(), "the loop is closed");
        for (const QPointF& v : loop[0]) MIB_EXPECT(std::abs(peak.lookup(v.x(), v.y()) - 5.5) < 1e-12, "loop vertex on level");
    }

    // ---- 4. hit testing ---------------------------------------------------------------------
    {
        const g::GateSpec spec = boxSpec();
        const g::EContours none;
        // x(area) = 50 + area, y(deform) = 20 + (0.5 - deform) * 800: box x 150..350, y 180..340.
        auto hit = [&](double px, double py, const g::GateSpec& s = boxSpec(), g::EContours c = {}) {
            return g::hitTest(s, c, vp, px, py);
        };
        MIB_EXPECT(hit(350, 260).kind == g::HandleKind::AreaMax && hit(350, 260).distancePx < 1e-9, "on the area-max edge");
        MIB_EXPECT(hit(355, 260).kind == g::HandleKind::AreaMax && std::abs(hit(355, 260).distancePx - 5.0) < 1e-9, "5 px away still hits");
        MIB_EXPECT(hit(357, 260).kind == g::HandleKind::None, "7 px away does not");
        MIB_EXPECT(hit(150, 260).kind == g::HandleKind::AreaMin, "area-min edge");
        MIB_EXPECT(hit(250, 180).kind == g::HandleKind::DeformMax, "deformability-max is the upper edge");
        MIB_EXPECT(hit(250, 340).kind == g::HandleKind::DeformMin, "deformability-min is the lower edge");
        MIB_EXPECT(hit(250, 260).kind == g::HandleKind::None, "inside the box, away from edges: nothing");
        MIB_EXPECT(hit(350, 100).kind == g::HandleKind::None, "an edge ends where the box ends");
        MIB_EXPECT(hit(152, 182).kind == g::HandleKind::AreaMin, "a corner: the tie goes to the area edge");
        MIB_EXPECT(hit(150, 260, spec).anchorPx == QPointF(150, 260), "anchor is the nearest point on the edge");
        g::GateSpec off = spec;
        off.enabled = false;
        MIB_EXPECT(hit(350, 260, off).kind == g::HandleKind::None, "gate off: no handles");
        MIB_EXPECT(g::hitTest(spec, none, vp, 40, 260).kind == g::HandleKind::None, "outside the plot: nothing");
        g::GateSpec same = boxSpec();
        same.areaMin = same.areaMax = 200;
        MIB_EXPECT(hit(252, 260, same).kind == g::HandleKind::AreaMax && hit(248, 260, same).kind == g::HandleKind::AreaMin,
                   "coincident area edges split by the pointer's side");
        g::GateSpec flat = boxSpec();
        flat.deformMin = flat.deformMax = 0.2;
        MIB_EXPECT(hit(250, 258, flat).kind == g::HandleKind::DeformMax && hit(250, 262, flat).kind == g::HandleKind::DeformMin,
                   "coincident deformability edges split by the pointer's side");
        // An edge outside the visible axis range cannot be grabbed.
        g::GateSpec far = boxSpec();
        far.areaMax = 900;
        MIB_EXPECT(g::handleAnchor(g::HandleKind::AreaMax, far, none, vp) == std::nullopt, "no anchor for an off-screen edge");

        // E contours.
        const g::GateSpec ge = [] {
            g::GateSpec s = boxSpec(true, 1.0, 3.0);
            s.areaMin = 0;
            s.areaMax = 600;
            s.deformMin = 0;
            s.deformMax = 0.5; // the box is the plot: only the contours are inside it
            return s;
        }();
        const g::Contour cMin = g::isoContour(lut, ge.emodMin);
        const g::Contour cMax = g::isoContour(lut, ge.emodMax);
        const g::EContours both{&cMin, &cMax};
        for (auto kind : {g::HandleKind::EmodMin, g::HandleKind::EmodMax}) {
            const auto anchor = g::handleAnchor(kind, ge, both, vp);
            MIB_REQUIRE(anchor.has_value(), "E contour has a visible anchor");
            const auto h = g::hitTest(ge, both, vp, anchor->x(), anchor->y());
            MIB_EXPECT(h.kind == kind && h.distancePx < 1e-6, "pointer on the contour hits it");
            const auto h2 = g::hitTest(ge, both, vp, anchor->x(), anchor->y() + 4.0);
            MIB_EXPECT(h2.kind == kind, "4 px off the contour still hits it");
        }
        g::GateSpec geOff = ge;
        geOff.emodEnabled = false;
        const auto anchorMin = g::handleAnchor(g::HandleKind::EmodMin, ge, both, vp);
        MIB_EXPECT(g::hitTest(geOff, both, vp, anchorMin->x(), anchorMin->y()).kind == g::HandleKind::None, "E gate off: no E handle");
        MIB_EXPECT(g::handleAnchor(g::HandleKind::EmodMin, geOff, both, vp) == std::nullopt, "E gate off: no E anchor");
        const g::EContours noContours;
        MIB_EXPECT(g::hitTest(ge, noContours, vp, anchorMin->x(), anchorMin->y()).kind == g::HandleKind::None, "no contour: no E handle");
        const auto label = g::contourLabelAnchor(cMin, vp);
        MIB_REQUIRE(label.has_value(), "label anchor");
        for (const auto& line : cMin)
            for (const QPointF& d : line) {
                const QPointF p = g::toPixel(vp, d);
                if (scatterhit::insidePlot(vp, p.x(), p.y())) MIB_EXPECT(p.x() <= label->x() + 1e-9, "label sits at the right-most visible vertex");
            }
        MIB_EXPECT(g::handleField(g::HandleKind::AreaMax) == TuneField::TargetAreaMax &&
                       g::handleField(g::HandleKind::DeformMin) == TuneField::TargetDeformabilityMin &&
                       g::handleField(g::HandleKind::EmodMax) == TuneField::TargetEmodulusMax,
                   "handles map to the tune fields");
    }

    // ---- 5. rounding, clamping, min <= max -------------------------------------------------------
    {
        MIB_EXPECT(g::roundToField(TuneField::TargetAreaMax, 190.6) == 191.0 && g::roundToField(TuneField::TargetAreaMax, 190.4) == 190.0,
                   "area rounds to whole um^2");
        MIB_EXPECT(g::roundToField(TuneField::TargetDeformabilityMax, 0.30437) == 0.30 && g::roundToField(TuneField::TargetDeformabilityMax, 0.3071) == 0.31,
                   "deformability rounds to 2 decimals");
        MIB_EXPECT(g::roundToField(TuneField::TargetEmodulusMin, 1.23456) == 1.23, "E rounds to 2 decimals");
        const g::GateSpec s = boxSpec(true, 1.0, 3.0);
        const g::Limits areaLim = g::defaultLimits(TuneField::TargetAreaMax);
        const g::Limits defLim = g::defaultLimits(TuneField::TargetDeformabilityMax);
        const g::Limits eLim = g::defaultLimits(TuneField::TargetEmodulusMax);
        MIB_EXPECT(areaLim.lo == 0 && areaLim.hi == 100000 && defLim.lo == 0 && defLim.hi == 1.0 && eLim.lo == 0 && eLim.hi == 1000,
                   "default limits are the tune panel's ranges");
        MIB_EXPECT(g::constrainEdit(g::HandleKind::AreaMax, 250.4, s, areaLim) == 250.0, "area edit rounds");
        MIB_EXPECT(g::constrainEdit(g::HandleKind::AreaMax, 50, s, areaLim) == 100.0, "area max cannot pass area min");
        MIB_EXPECT(g::constrainEdit(g::HandleKind::AreaMin, 400, s, areaLim) == 300.0, "area min cannot pass area max");
        MIB_EXPECT(g::constrainEdit(g::HandleKind::AreaMin, -20, s, areaLim) == 0.0, "area min clamps at the control's minimum");
        MIB_EXPECT(g::constrainEdit(g::HandleKind::AreaMax, 1e9, s, areaLim) == 100000.0, "area max clamps at the control's maximum");
        MIB_EXPECT(g::constrainEdit(g::HandleKind::DeformMax, 1.7, s, defLim) == 1.0, "deformability clamps at 1");
        MIB_EXPECT(g::constrainEdit(g::HandleKind::DeformMax, 0.05, s, defLim) == 0.10, "deformability max cannot pass min");
        MIB_EXPECT(g::constrainEdit(g::HandleKind::DeformMin, 0.298, s, defLim) == 0.30, "deformability min stops at max, rounded");
        MIB_EXPECT(g::constrainEdit(g::HandleKind::EmodMin, 5.0, s, eLim) == 3.0, "E min cannot pass E max");
        MIB_EXPECT(g::constrainEdit(g::HandleKind::EmodMax, 0.2, s, eLim) == 1.0, "E max cannot pass E min");
        MIB_EXPECT(g::constrainEdit(g::HandleKind::EmodMax, 2000, s, eLim) == 1000.0, "E max clamps at the control's maximum");
        // A narrower control range wins over the defaults.
        MIB_EXPECT(g::constrainEdit(g::HandleKind::AreaMax, 5000, s, g::Limits{0, 1000}) == 1000.0, "limits come from the caller");
    }

    // ---- 6. dragging ------------------------------------------------------------------------------------
    {
        const g::GateSpec s = boxSpec();
        const g::EContours none;
        auto start = [&](g::HandleKind kind, QPointF press, const g::GateSpec& spec = boxSpec(), g::EContours c = {}) {
            const auto h = g::hitTest(spec, c, vp, press.x(), press.y());
            MIB_REQUIRE(h.kind == kind, "drag starts on the expected handle");
            return g::beginDrag(h, spec, &lut, vp, press);
        };
        const g::Limits areaLim = g::defaultLimits(TuneField::TargetAreaMax);
        const g::Limits defLim = g::defaultLimits(TuneField::TargetDeformabilityMax);
        auto sessionMax = start(g::HandleKind::AreaMax, QPointF(353, 260)); // pressed 3 px beside the edge
        MIB_EXPECT(sessionMax.valueAtPress == 300.0, "the session remembers the value at the press");
        MIB_EXPECT(g::dragValue(sessionMax, s, &lut, vp, QPointF(353, 260), areaLim) == 300.0, "no travel, no jump");
        MIB_EXPECT(g::dragValue(sessionMax, s, &lut, vp, QPointF(383, 260), areaLim) == 330.0, "30 px right = +30 um^2 (relative to the press)");
        MIB_EXPECT(g::dragValue(sessionMax, s, &lut, vp, QPointF(383, 400), areaLim) == 330.0, "vertical travel does not matter for an area edge");
        MIB_EXPECT(g::dragValue(sessionMax, s, &lut, vp, QPointF(60, 260), areaLim) == 100.0, "dragged past the minimum: stops there");
        auto sessionDef = start(g::HandleKind::DeformMax, QPointF(250, 183));
        MIB_EXPECT(g::dragValue(sessionDef, s, &lut, vp, QPointF(250, 183), defLim) == 0.30, "no travel, no jump (deformability)");
        MIB_EXPECT(g::dragValue(sessionDef, s, &lut, vp, QPointF(250, 103), defLim) == 0.40, "80 px up = +0.10 deformability (y grows downwards)");
        MIB_EXPECT(g::dragValue(sessionDef, s, &lut, vp, QPointF(250, 186.7), defLim) == 0.30, "a 3.7 px move rounds back to the displayed value");
        MIB_EXPECT(g::dragValue(sessionDef, s, &lut, vp, QPointF(250, 600), defLim) == 0.10, "dragged below the minimum: stops there");
        MIB_EXPECT(g::dragValue(sessionDef, s, &lut, vp, QPointF(0, 103), defLim) == 0.40, "horizontal travel does not matter for a deformability edge");
        auto sessionMin = start(g::HandleKind::DeformMin, QPointF(250, 338));
        MIB_EXPECT(g::dragValue(sessionMin, s, &lut, vp, QPointF(250, 298), defLim) == 0.15, "40 px up on the lower edge = +0.05");
        MIB_EXPECT(g::dragValue(sessionMin, s, &lut, vp, QPointF(250, 100), defLim) == 0.30, "min stops at max");
        // A draft value finer than the display (0.123456) is only rewritten by an actual move.
        g::GateSpec fine = boxSpec();
        fine.deformMin = 0.123456;
        auto sessionFine = start(g::HandleKind::DeformMin, QPointF(250, 20 + (0.5 - 0.123456) * 800.0), fine);
        MIB_EXPECT(sessionFine.valueAtPress == 0.123456, "the session starts from the exact draft value");
        MIB_EXPECT(g::dragValue(sessionFine, fine, &lut, vp, QPointF(250, 20 + (0.5 - 0.123456) * 800.0 - 8.0), defLim) == 0.13,
                   "8 px up from 0.123456 -> 0.133456 -> 0.13");

        // E contour: relative to the press, along the table's gradient.
        const g::GateSpec ge = [] {
            g::GateSpec e = boxSpec(true, 2.0, 4.0);
            e.areaMin = 0;
            e.areaMax = 600;
            e.deformMin = 0;
            e.deformMax = 0.5;
            return e;
        }();
        const g::Contour cMin = g::isoContour(lut, 2.0);
        const g::Contour cMax = g::isoContour(lut, 4.0);
        const g::EContours both{&cMin, &cMax};
        const g::Limits eLim = g::defaultLimits(TuneField::TargetEmodulusMin);
        const auto press = *g::handleAnchor(g::HandleKind::EmodMin, ge, both, vp);
        auto sessionE = start(g::HandleKind::EmodMin, press, ge, both);
        MIB_EXPECT(sessionE.valueAtPress == 2.0 && std::abs(sessionE.eAtAnchor - 2.0) < 0.05, "E session starts at the contour level");
        MIB_EXPECT(g::dragValue(sessionE, ge, &lut, vp, press, eLim) == 2.0, "no travel, no jump (E)");
        // Moving right (larger area) raises E; E min stops at E max (4.0); left lowers it.
        const auto up = g::dragValue(sessionE, ge, &lut, vp, press + QPointF(20, 0), eLim);
        const auto down = g::dragValue(sessionE, ge, &lut, vp, press + QPointF(-20, 0), eLim);
        MIB_REQUIRE(up && down, "E drag has values inside the table");
        MIB_EXPECT(*up > 2.0 && *down < 2.0, "E follows the table under the contour");
        MIB_EXPECT(std::abs(*up - (2.0 + 0.2 * (1.0 - 2.0 * g::toData(vp, press).y()))) < 0.011, "20 px = 20 um^2 -> dE = 0.2 * (1 - 2 d)");
        MIB_EXPECT(std::abs(*up * 100 - std::round(*up * 100)) < 1e-9, "E value has 2 decimals");
        // A pointer position whose carried contour point has E ~ 6.3 (area 580, deformability 0): E min stops at E max.
        const QPointF high = g::toPixel(vp, QPointF(580.0, 0.0)) - sessionE.anchorOffsetPx;
        const auto capped = g::dragValue(sessionE, ge, &lut, vp, high, eLim);
        MIB_REQUIRE(capped.has_value(), "that position is inside the table");
        MIB_EXPECT(*capped == 4.0, "E min stops at E max");
        // Beyond the table there is no E: keep the last value.
        g::Viewport wide = vp;
        wide.x1 = 1200.0; // the plot now shows 0..1200 um^2; the table ends at 600
        wide.width = 1200.0;
        const g::HandleHit h = g::hitTest(ge, both, wide, press.x(), press.y());
        MIB_REQUIRE(h.kind == g::HandleKind::EmodMin, "contour still there on the wider plot");
        const auto sessionWide = g::beginDrag(h, ge, &lut, wide, press);
        MIB_EXPECT(!g::dragValue(sessionWide, ge, &lut, wide, QPointF(wide.left + 1100, press.y()), eLim).has_value(),
                   "pointer beyond the table: no value, the edit is skipped");
        MIB_EXPECT(!g::dragValue(sessionE, ge, nullptr, vp, press + QPointF(20, 0), eLim).has_value(), "no table: no E drag");
        MIB_EXPECT(!g::dragValue(g::DragSession{}, ge, &lut, vp, press, eLim).has_value(), "no handle: no value");
    }

    // ---- 7. live count ------------------------------------------------------------------------------------------
    {
        std::vector<scatterhit::Point> pts = {{150, 0.2, 1}, {350, 0.2, 2}, {200, 0.05, 3}, {200, 0.25, 4}, {250, 0.15, 5}};
        std::vector<double> e = {2.0, 2.0, 2.0, kNaN, 5.0};
        // box only: 1, 4, 5 are inside (350 is out, 0.05 is out)
        auto c = g::countTargets(boxSpec(false), true, pts, e);
        MIB_EXPECT(c.target == 3 && c.total == 5 && std::abs(c.percent() - 60.0) < 1e-9, "box-only count (E ignored, incl. NaN)");
        // with E in [1, 3] and a table: cell 4 has no E, cell 5 has E 5 -> only cell 1
        c = g::countTargets(boxSpec(true, 1.0, 3.0), true, pts, e);
        MIB_EXPECT(c.target == 1 && c.total == 5, "E gate, table: NaN and out-of-range E are not target");
        g::GateSpec off = boxSpec(true, 1.0, 3.0);
        off.enabled = false;
        MIB_EXPECT(g::countTargets(off, true, pts, e).target == 0 && g::countTargets(off, false, pts, e).target == 0, "gate off: none");
        MIB_EXPECT(g::countTargets(boxSpec(true, 1.0, 3.0), true, pts, std::vector<double>{}).target == 0,
                   "table loaded but no E values: an E gate admits none");
        // No table: the E layer is skipped, the count is the box (cells 1, 4, 5), whatever the stored E (a 0.0
        // placeholder, NaN, or values) and whatever the E range (min 0 or positive).
        const std::vector<double> placeholder(pts.size(), 0.0);
        for (const double eMin : {0.0, 5.0}) {
            for (const std::vector<double>* values : std::vector<const std::vector<double>*>{&e, &placeholder}) {
                c = g::countTargets(boxSpec(true, eMin, 10.0), false, pts, *values);
                MIB_EXPECT(c.target == 3 && c.total == 5, "E gate, no table: the count is the box (E min " + std::to_string(eMin) + ")");
            }
            MIB_EXPECT(g::countTargets(boxSpec(true, eMin, 10.0), false, pts, std::vector<double>{}).target == 3,
                       "E gate, no table, no E values: the count is the box");
        }
        MIB_EXPECT(g::countTargets(boxSpec(false), true, {}, {}).total == 0 && g::countTargets(boxSpec(false), true, {}, {}).percent() == 0.0, "empty set");
        MIB_EXPECT(g::previewText({3, 5}) == QStringLiteral("Target group preview: 3 of 5 plotted valid cells (60.0%)"), "preview text");
        MIB_EXPECT(g::previewText({0, 0}).contains(QStringLiteral("no plotted valid cells")), "preview text without cells");
    }

    return mib::test::exitCode();
}
