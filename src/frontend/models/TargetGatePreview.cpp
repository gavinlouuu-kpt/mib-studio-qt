#include "frontend/models/TargetGatePreview.h"

#include <QObject>

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace frontend::gate {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

double distanceToSegment(const QPointF& p, const QPointF& a, const QPointF& b, QPointF& nearest)
{
    const double dx = b.x() - a.x();
    const double dy = b.y() - a.y();
    const double len2 = dx * dx + dy * dy;
    double t = 0.0;
    if (len2 > 0.0) t = std::clamp(((p.x() - a.x()) * dx + (p.y() - a.y()) * dy) / len2, 0.0, 1.0);
    nearest = QPointF(a.x() + t * dx, a.y() + t * dy);
    return std::hypot(p.x() - nearest.x(), p.y() - nearest.y());
}

bool inPlot(const Viewport& v, const QPointF& p)
{
    return scatterhit::insidePlot(v, p.x(), p.y());
}

} // namespace

// ---- the gate -----------------------------------------------------------------

GateSpec GateSpec::fromConfig(const backend::services::ProcessingConfig& cfg)
{
    GateSpec s;
    s.enabled = cfg.enable_target_group;
    s.areaMin = cfg.target_group_area_min;
    s.areaMax = cfg.target_group_area_max;
    s.deformMin = cfg.target_group_deformability_min;
    s.deformMax = cfg.target_group_deformability_max;
    s.emodEnabled = cfg.enable_target_group_emodulus;
    s.emodMin = cfg.target_group_emodulus_min;
    s.emodMax = cfg.target_group_emodulus_max;
    return s;
}

bool GateSpec::operator==(const GateSpec& o) const
{
    return enabled == o.enabled && areaMin == o.areaMin && areaMax == o.areaMax && deformMin == o.deformMin &&
           deformMax == o.deformMax && emodEnabled == o.emodEnabled && emodMin == o.emodMin && emodMax == o.emodMax;
}

double lookupE(const backend::EModulusLut* lut, double areaUm, double deformability)
{
    if (!lut || !lut->isLoaded()) return kNaN;
    return lut->lookup(areaUm, deformability);
}

bool isTarget(const GateSpec& s, double areaUm, double deformability, double emodulus)
{
    // Mirror of ProcessingScience.cpp (tgArea / tgDeform / tgEmod) for a valid cell.
    if (!s.enabled) return false;
    const bool tgArea = (areaUm >= s.areaMin && areaUm <= s.areaMax);
    const bool tgDeform = (deformability >= s.deformMin && deformability <= s.deformMax);
    const bool tgEmod = !s.emodEnabled || (!std::isnan(emodulus) && emodulus >= s.emodMin && emodulus <= s.emodMax);
    return tgArea && tgDeform && tgEmod;
}

bool wouldBeTarget(const GateSpec& s, const backend::EModulusLut* lut, double areaUm, double deformability)
{
    // E is only consulted with the E gate on; skip the lookup otherwise.
    const double e = s.enabled && s.emodEnabled ? lookupE(lut, areaUm, deformability) : kNaN;
    return isTarget(s, areaUm, deformability, e);
}

// ---- data <-> pixel -------------------------------------------------------------

bool viewportValid(const Viewport& v)
{
    return v.x1 > v.x0 && v.y1 > v.y0 && v.width > 0.0 && v.height > 0.0 && std::isfinite(v.x0) && std::isfinite(v.x1) &&
           std::isfinite(v.y0) && std::isfinite(v.y1);
}

QPointF toPixel(const Viewport& v, const QPointF& d)
{
    return QPointF(v.left + (d.x() - v.x0) * v.width / (v.x1 - v.x0), v.top + (v.y1 - d.y()) * v.height / (v.y1 - v.y0));
}

QPointF toData(const Viewport& v, const QPointF& p)
{
    return QPointF(v.x0 + (p.x() - v.left) * (v.x1 - v.x0) / v.width, v.y1 - (p.y() - v.top) * (v.y1 - v.y0) / v.height);
}

// ---- tinted region ----------------------------------------------------------------

std::size_t Mask::onCount() const
{
    return static_cast<std::size_t>(std::count(on.begin(), on.end(), std::uint8_t{1}));
}

bool Mask::at(int col, int row) const
{
    if (col < 0 || row < 0 || col >= cols || row >= rows) return false;
    return on[static_cast<std::size_t>(row) * static_cast<std::size_t>(cols) + static_cast<std::size_t>(col)] != 0;
}

Mask computeMask(const GateSpec& spec, const backend::EModulusLut* lut, const Viewport& vp, double stepPx)
{
    Mask m;
    m.stepPx = stepPx > 0.5 ? stepPx : 0.5;
    if (!viewportValid(vp)) return m;
    m.cols = static_cast<int>(std::ceil(vp.width / m.stepPx));
    m.rows = static_cast<int>(std::ceil(vp.height / m.stepPx));
    m.on.assign(static_cast<std::size_t>(m.cols) * static_cast<std::size_t>(m.rows), 0);
    if (!spec.enabled) return m;
    for (int r = 0; r < m.rows; ++r) {
        const double py = vp.top + (r + 0.5) * m.stepPx;
        if (py > vp.top + vp.height) continue;
        for (int c = 0; c < m.cols; ++c) {
            const double px = vp.left + (c + 0.5) * m.stepPx;
            if (px > vp.left + vp.width) continue;
            const QPointF d = toData(vp, QPointF(px, py));
            if (wouldBeTarget(spec, lut, d.x(), d.y()))
                m.on[static_cast<std::size_t>(r) * static_cast<std::size_t>(m.cols) + static_cast<std::size_t>(c)] = 1;
        }
    }
    return m;
}

// ---- iso-E contours ---------------------------------------------------------------

Contour isoContour(const backend::EModulusLut& lut, double level)
{
    Contour out;
    if (!lut.isLoaded() || !std::isfinite(level)) return out;
    const std::size_t na = lut.gridAreaBins();
    const std::size_t nd = lut.gridDeformBins();
    const auto& g = lut.grid();
    if (na < 2 || nd < 2 || g.size() != na * nd) return out;
    const double a0 = lut.gridAreaMin();
    const double da = lut.gridAreaStep();
    const double d0 = lut.gridDeformMin();
    const double dd = lut.gridDeformStep();

    auto value = [&](std::size_t ai, std::size_t di) { return g[di * na + ai]; };
    auto inside = [&](double v) { return v >= level; };

    // A crossing is identified by the grid edge it lies on: bit 0 = vertical
    // edge ((ai, di) - (ai, di + 1)), else horizontal ((ai, di) - (ai + 1, di)).
    auto edgeId = [&](std::size_t ai, std::size_t di, bool vertical) {
        return (static_cast<std::uint64_t>(di * na + ai) << 1) | (vertical ? 1u : 0u);
    };
    auto crossing = [&](std::uint64_t id) {
        const bool vertical = (id & 1u) != 0;
        const std::size_t cell = static_cast<std::size_t>(id >> 1);
        const std::size_t ai = cell % na;
        const std::size_t di = cell / na;
        const std::size_t aj = vertical ? ai : ai + 1;
        const std::size_t dj = vertical ? di + 1 : di;
        const double fp = value(ai, di);
        const double fq = value(aj, dj);
        const double t = (level - fp) / (fq - fp);
        return QPointF(a0 + da * (static_cast<double>(ai) + t * static_cast<double>(aj - ai)),
                       d0 + dd * (static_cast<double>(di) + t * static_cast<double>(dj - di)));
    };

    struct Node {
        std::array<std::uint64_t, 2> next{};
        int degree{0};
    };
    std::unordered_map<std::uint64_t, Node> graph;
    auto link = [&](std::uint64_t a, std::uint64_t b) {
        Node& na_ = graph[a];
        if (na_.degree < 2) na_.next[static_cast<std::size_t>(na_.degree++)] = b;
        Node& nb_ = graph[b];
        if (nb_.degree < 2) nb_.next[static_cast<std::size_t>(nb_.degree++)] = a;
    };

    for (std::size_t di = 0; di + 1 < nd; ++di) {
        for (std::size_t ai = 0; ai + 1 < na; ++ai) {
            const double f0 = value(ai, di);          // lower-left
            const double f1 = value(ai + 1, di);      // lower-right
            const double f2 = value(ai + 1, di + 1);  // upper-right
            const double f3 = value(ai, di + 1);      // upper-left
            if (std::isnan(f0) || std::isnan(f1) || std::isnan(f2) || std::isnan(f3)) continue;
            const bool s0 = inside(f0), s1 = inside(f1), s2 = inside(f2), s3 = inside(f3);
            if (s0 == s1 && s1 == s2 && s2 == s3) continue;
            const std::uint64_t e0 = edgeId(ai, di, false);      // c0 - c1
            const std::uint64_t e1 = edgeId(ai + 1, di, true);   // c1 - c2
            const std::uint64_t e2 = edgeId(ai, di + 1, false);  // c3 - c2
            const std::uint64_t e3 = edgeId(ai, di, true);       // c0 - c3
            std::array<std::uint64_t, 4> crossed{};
            int n = 0;
            if (s0 != s1) crossed[static_cast<std::size_t>(n++)] = e0;
            if (s1 != s2) crossed[static_cast<std::size_t>(n++)] = e1;
            if (s3 != s2) crossed[static_cast<std::size_t>(n++)] = e2;
            if (s0 != s3) crossed[static_cast<std::size_t>(n++)] = e3;
            if (n == 2) {
                link(crossed[0], crossed[1]);
            } else if (n == 4) {
                // Saddle: the bilinear centre value decides which diagonal pair is connected.
                const bool centre = inside(0.25 * (f0 + f1 + f2 + f3));
                if (centre == s0) {
                    link(e0, e1); // cut off c1
                    link(e2, e3); // cut off c3
                } else {
                    link(e0, e3); // cut off c0
                    link(e1, e2); // cut off c2
                }
            }
        }
    }

    std::unordered_set<std::uint64_t> visited;
    auto walk = [&](std::uint64_t start) {
        Polyline line;
        std::uint64_t cur = start;
        while (true) {
            visited.insert(cur);
            line.push_back(crossing(cur));
            const Node& node = graph.at(cur);
            bool advanced = false;
            for (int i = 0; i < node.degree; ++i) {
                const std::uint64_t cand = node.next[static_cast<std::size_t>(i)];
                if (visited.count(cand)) continue;
                cur = cand;
                advanced = true;
                break;
            }
            if (!advanced) break;
        }
        return line;
    };
    // Open lines first (they end on degree-1 crossings at the grid boundary), then closed loops.
    for (const auto& [id, node] : graph) {
        if (node.degree == 1 && !visited.count(id)) {
            Polyline line = walk(id);
            if (line.size() >= 2) out.push_back(std::move(line));
        }
    }
    for (const auto& [id, node] : graph) {
        (void)node;
        if (!visited.count(id)) {
            Polyline line = walk(id);
            if (line.size() >= 3) line.push_back(line.front()); // close the loop
            if (line.size() >= 2) out.push_back(std::move(line));
        }
    }
    return out;
}

// ---- handles ------------------------------------------------------------------------

TuneField handleField(HandleKind kind)
{
    switch (kind) {
    case HandleKind::AreaMin: return TuneField::TargetAreaMin;
    case HandleKind::AreaMax: return TuneField::TargetAreaMax;
    case HandleKind::DeformMin: return TuneField::TargetDeformabilityMin;
    case HandleKind::DeformMax: return TuneField::TargetDeformabilityMax;
    case HandleKind::EmodMin: return TuneField::TargetEmodulusMin;
    case HandleKind::EmodMax: return TuneField::TargetEmodulusMax;
    case HandleKind::None: break;
    }
    return TuneField::TargetGroupEnabled;
}

bool isVerticalEdge(HandleKind kind) { return kind == HandleKind::AreaMin || kind == HandleKind::AreaMax; }
bool isHorizontalEdge(HandleKind kind) { return kind == HandleKind::DeformMin || kind == HandleKind::DeformMax; }
bool isEmodHandle(HandleKind kind) { return kind == HandleKind::EmodMin || kind == HandleKind::EmodMax; }

double handleValue(const GateSpec& s, HandleKind kind)
{
    switch (kind) {
    case HandleKind::AreaMin: return s.areaMin;
    case HandleKind::AreaMax: return s.areaMax;
    case HandleKind::DeformMin: return s.deformMin;
    case HandleKind::DeformMax: return s.deformMax;
    case HandleKind::EmodMin: return s.emodMin;
    case HandleKind::EmodMax: return s.emodMax;
    case HandleKind::None: break;
    }
    return kNaN;
}

HandleHit hitTest(const GateSpec& spec, const EContours& contours, const Viewport& vp, double px, double py,
                  double tol)
{
    HandleHit best;
    if (!spec.enabled || !viewportValid(vp) || !scatterhit::insidePlot(vp, px, py)) return best;
    const double right = vp.left + vp.width;
    const double bottom = vp.top + vp.height;
    const QPointF p(px, py);

    auto consider = [&](HandleKind kind, double distance, const QPointF& anchor) {
        if (distance <= tol && distance < best.distancePx) {
            best.kind = kind;
            best.distancePx = distance;
            best.anchorPx = anchor;
        }
    };

    const double xMin = toPixel(vp, QPointF(spec.areaMin, 0)).x();
    const double xMax = toPixel(vp, QPointF(spec.areaMax, 0)).x();
    const double yTop = toPixel(vp, QPointF(0, spec.deformMax)).y(); // deformability maximum: upper edge
    const double yBottom = toPixel(vp, QPointF(0, spec.deformMin)).y();
    // The box edges, clipped to the plot rectangle.
    const double vy0 = std::max(yTop, vp.top), vy1 = std::min(yBottom, bottom);
    const double hx0 = std::max(xMin, vp.left), hx1 = std::min(xMax, right);

    auto vertical = [&](HandleKind kind, double x) {
        if (x < vp.left || x > right || vy0 > vy1) return;
        const double cy = std::clamp(py, vy0, vy1);
        consider(kind, std::hypot(px - x, py - cy), QPointF(x, cy));
    };
    auto horizontal = [&](HandleKind kind, double y) {
        if (y < vp.top || y > bottom || hx0 > hx1) return;
        const double cx = std::clamp(px, hx0, hx1);
        consider(kind, std::hypot(px - cx, py - y), QPointF(cx, y));
    };
    if (std::abs(xMax - xMin) < 1.0) vertical(px >= xMax ? HandleKind::AreaMax : HandleKind::AreaMin, xMax);
    else {
        vertical(HandleKind::AreaMin, xMin);
        vertical(HandleKind::AreaMax, xMax);
    }
    if (std::abs(yBottom - yTop) < 1.0) horizontal(py <= yTop ? HandleKind::DeformMax : HandleKind::DeformMin, yTop);
    else {
        horizontal(HandleKind::DeformMin, yBottom);
        horizontal(HandleKind::DeformMax, yTop);
    }

    auto contourHit = [&](HandleKind kind, const Contour* contour) {
        if (!contour) return;
        for (const Polyline& line : *contour) {
            for (std::size_t i = 0; i + 1 < line.size(); ++i) {
                QPointF nearest;
                const double d = distanceToSegment(p, toPixel(vp, line[i]), toPixel(vp, line[i + 1]), nearest);
                if (d <= tol && inPlot(vp, nearest)) consider(kind, d, nearest);
            }
        }
    };
    if (spec.emodEnabled) {
        contourHit(HandleKind::EmodMin, contours.min);
        contourHit(HandleKind::EmodMax, contours.max);
    }
    return best;
}

std::optional<QPointF> handleAnchor(HandleKind kind, const GateSpec& spec, const EContours& contours, const Viewport& vp)
{
    if (!spec.enabled || !viewportValid(vp) || kind == HandleKind::None) return std::nullopt;
    const double right = vp.left + vp.width;
    const double bottom = vp.top + vp.height;
    const double xMin = toPixel(vp, QPointF(spec.areaMin, 0)).x();
    const double xMax = toPixel(vp, QPointF(spec.areaMax, 0)).x();
    const double yTop = toPixel(vp, QPointF(0, spec.deformMax)).y();
    const double yBottom = toPixel(vp, QPointF(0, spec.deformMin)).y();
    const double vy0 = std::max(yTop, vp.top), vy1 = std::min(yBottom, bottom);
    const double hx0 = std::max(xMin, vp.left), hx1 = std::min(xMax, right);

    if (isVerticalEdge(kind)) {
        const double x = kind == HandleKind::AreaMin ? xMin : xMax;
        if (x < vp.left || x > right || vy0 > vy1) return std::nullopt;
        return QPointF(x, 0.5 * (vy0 + vy1));
    }
    if (isHorizontalEdge(kind)) {
        const double y = kind == HandleKind::DeformMin ? yBottom : yTop;
        if (y < vp.top || y > bottom || hx0 > hx1) return std::nullopt;
        return QPointF(0.5 * (hx0 + hx1), y);
    }
    if (!spec.emodEnabled) return std::nullopt;
    const Contour* contour = kind == HandleKind::EmodMin ? contours.min : contours.max;
    if (!contour) return std::nullopt;
    std::vector<QPointF> best;
    for (const Polyline& line : *contour) {
        std::vector<QPointF> visible;
        for (const QPointF& d : line) {
            const QPointF p = toPixel(vp, d);
            if (inPlot(vp, p)) visible.push_back(p);
        }
        if (visible.size() > best.size()) best = std::move(visible);
    }
    if (best.empty()) return std::nullopt;
    return best[best.size() / 2];
}

std::optional<QPointF> contourLabelAnchor(const Contour& contour, const Viewport& vp)
{
    if (!viewportValid(vp)) return std::nullopt;
    std::optional<QPointF> best;
    for (const Polyline& line : contour) {
        for (const QPointF& d : line) {
            const QPointF p = toPixel(vp, d);
            if (!inPlot(vp, p)) continue;
            if (!best || p.x() > best->x()) best = p;
        }
    }
    return best;
}

// ---- editing ------------------------------------------------------------------------------

Limits defaultLimits(TuneField field)
{
    switch (field) {
    case TuneField::TargetAreaMin:
    case TuneField::TargetAreaMax: return {0.0, 100000.0};
    case TuneField::TargetDeformabilityMin:
    case TuneField::TargetDeformabilityMax: return {0.0, 1.0};
    case TuneField::TargetEmodulusMin:
    case TuneField::TargetEmodulusMax: return {0.0, 1000.0};
    default: break;
    }
    return {0.0, 0.0};
}

double roundToField(TuneField field, double value)
{
    const int decimals = tuneFieldDecimals(field);
    if (decimals <= 0) return std::round(value);
    const double scale = std::pow(10.0, decimals);
    return std::round(value * scale) / scale;
}

double constrainEdit(HandleKind kind, double raw, const GateSpec& current, const Limits& limits)
{
    double lo = limits.lo;
    double hi = limits.hi;
    switch (kind) {
    case HandleKind::AreaMin: hi = std::min(hi, current.areaMax); break;
    case HandleKind::AreaMax: lo = std::max(lo, current.areaMin); break;
    case HandleKind::DeformMin: hi = std::min(hi, current.deformMax); break;
    case HandleKind::DeformMax: lo = std::max(lo, current.deformMin); break;
    case HandleKind::EmodMin: hi = std::min(hi, current.emodMax); break;
    case HandleKind::EmodMax: lo = std::max(lo, current.emodMin); break;
    case HandleKind::None: return raw;
    }
    if (hi < lo) hi = lo; // the other bound lies outside the limits: pin to the near limit
    return roundToField(handleField(kind), std::clamp(raw, lo, hi));
}

DragSession beginDrag(const HandleHit& hit, const GateSpec& spec, const backend::EModulusLut* lut, const Viewport& vp,
                      const QPointF& pressPx)
{
    DragSession s;
    s.kind = hit.kind;
    s.pressPx = pressPx;
    s.valueAtPress = handleValue(spec, hit.kind);
    if (isEmodHandle(hit.kind)) {
        s.anchorOffsetPx = hit.anchorPx - pressPx;
        const QPointF d = toData(vp, hit.anchorPx);
        s.eAtAnchor = lookupE(lut, d.x(), d.y());
    }
    return s;
}

std::optional<double> dragValue(const DragSession& s, const GateSpec& current, const backend::EModulusLut* lut,
                                const Viewport& vp, const QPointF& nowPx, const Limits& limits)
{
    if (s.kind == HandleKind::None || !viewportValid(vp)) return std::nullopt;
    double raw = s.valueAtPress;
    if (isVerticalEdge(s.kind)) {
        raw += (nowPx.x() - s.pressPx.x()) * (vp.x1 - vp.x0) / vp.width;
    } else if (isHorizontalEdge(s.kind)) {
        raw -= (nowPx.y() - s.pressPx.y()) * (vp.y1 - vp.y0) / vp.height;
    } else {
        const QPointF d = toData(vp, nowPx + s.anchorOffsetPx);
        const double e = lookupE(lut, d.x(), d.y());
        if (std::isnan(e) || std::isnan(s.eAtAnchor)) return std::nullopt;
        raw += e - s.eAtAnchor;
    }
    return constrainEdit(s.kind, raw, current, limits);
}

// ---- live count -------------------------------------------------------------------------------

PreviewCount countTargets(const GateSpec& spec, const std::vector<scatterhit::Point>& points,
                          const std::vector<double>& emodulus)
{
    PreviewCount c;
    c.total = static_cast<int>(points.size());
    for (std::size_t i = 0; i < points.size(); ++i) {
        const double e = i < emodulus.size() ? emodulus[i] : kNaN;
        if (isTarget(spec, points[i].x, points[i].y, e)) ++c.target;
    }
    return c;
}

QString previewText(const PreviewCount& c)
{
    if (c.total <= 0) return QObject::tr("Target group preview: no plotted valid cells yet");
    return QObject::tr("Target group preview: %1 of %2 plotted valid cells (%3%)")
        .arg(c.target)
        .arg(c.total)
        .arg(c.percent(), 0, 'f', 1);
}

} // namespace frontend::gate
