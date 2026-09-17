#include "backend/processing/DotGridDecoder.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <map>
#include <numeric>
#include <random>

namespace backend::dotgrid {

namespace {

constexpr int kMaxPhaseCandidates = 4;
constexpr int kBasisSubsample = 400;
constexpr int kFitIterations = 4;

// (a, b, c, d): u' = a*u + b*v, v' = c*u + d*v. Rotations first, then mirrors.
constexpr std::array<std::array<int, 4>, 8> kDihedral = {{{1, 0, 0, 1},
                                                          {0, -1, 1, 0},
                                                          {-1, 0, 0, -1},
                                                          {0, 1, -1, 0},
                                                          {-1, 0, 0, 1},
                                                          {1, 0, 0, -1},
                                                          {0, 1, 1, 0},
                                                          {0, -1, -1, 0}}};

struct Affine {
    // x = a*u + b*v + tx ; y = c*u + d*v + ty
    double a{1}, b{0}, tx{0}, c{0}, d{1}, ty{0};
    cv::Point2d apply(double u, double v) const { return {a * u + b * v + tx, c * u + d * v + ty}; }
    double det() const { return a * d - b * c; }
    bool invertLinear(double& ia, double& ib, double& ic, double& id) const {
        const double dt = det();
        if (std::abs(dt) < 1e-12) return false;
        ia = d / dt;
        ib = -b / dt;
        ic = -c / dt;
        id = a / dt;
        return true;
    }
};

// Least squares affine (u,v) -> (x,y). Returns false if degenerate.
bool fitAffine(const std::vector<cv::Point2d>& uv, const std::vector<cv::Point2d>& xy,
               Affine& out) {
    const int n = static_cast<int>(uv.size());
    if (n < 3) return false;
    cv::Mat A(n, 3, CV_64F);
    cv::Mat bx(n, 1, CV_64F), by(n, 1, CV_64F);
    for (int i = 0; i < n; ++i) {
        A.at<double>(i, 0) = uv[static_cast<size_t>(i)].x;
        A.at<double>(i, 1) = uv[static_cast<size_t>(i)].y;
        A.at<double>(i, 2) = 1.0;
        bx.at<double>(i, 0) = xy[static_cast<size_t>(i)].x;
        by.at<double>(i, 0) = xy[static_cast<size_t>(i)].y;
    }
    cv::Mat sx, sy;
    if (!cv::solve(A, bx, sx, cv::DECOMP_SVD) || !cv::solve(A, by, sy, cv::DECOMP_SVD))
        return false;
    out.a = sx.at<double>(0);
    out.b = sx.at<double>(1);
    out.tx = sx.at<double>(2);
    out.c = sy.at<double>(0);
    out.d = sy.at<double>(1);
    out.ty = sy.at<double>(2);
    return std::isfinite(out.det()) && std::abs(out.det()) > 1e-9;
}

bool estimateBasis(std::vector<cv::Point2f> pts, cv::Point2d& aOut, cv::Point2d& bOut) {
    if (pts.size() < 8) return false;
    if (pts.size() > static_cast<size_t>(kBasisSubsample)) {
        cv::Point2f c(0, 0);
        for (const auto& p : pts)
            c += p;
        c *= 1.0f / static_cast<float>(pts.size());
        std::nth_element(pts.begin(), pts.begin() + kBasisSubsample, pts.end(),
                         [&](const cv::Point2f& p, const cv::Point2f& q) {
                             const auto dp = p - c, dq = q - c;
                             return dp.dot(dp) < dq.dot(dq);
                         });
        pts.resize(static_cast<size_t>(kBasisSubsample));
    }
    const size_t n = pts.size();
    // 4 nearest neighbours per point
    std::vector<cv::Point2d> vecs;
    vecs.reserve(n * 4);
    std::vector<std::pair<double, size_t>> dist(n);
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j < n; ++j) {
            const auto d = pts[j] - pts[i];
            dist[j] = {i == j ? std::numeric_limits<double>::infinity() : double(d.dot(d)), j};
        }
        std::partial_sort(dist.begin(), dist.begin() + 4, dist.end());
        for (int k = 0; k < 4; ++k) {
            const auto d = pts[dist[static_cast<size_t>(k)].second] - pts[i];
            vecs.emplace_back(d.x, d.y);
        }
    }
    std::vector<double> lens(vecs.size());
    for (size_t i = 0; i < vecs.size(); ++i)
        lens[i] = std::hypot(vecs[i].x, vecs[i].y);
    std::vector<double> sorted = lens;
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<long>(sorted.size() / 2),
                     sorted.end());
    const double p0 = sorted[sorted.size() / 2];
    if (!(p0 > 1.0)) return false;
    double s4 = 0, c4 = 0;
    for (size_t i = 0; i < vecs.size(); ++i) {
        if (lens[i] < 0.6 * p0 || lens[i] > 1.4 * p0) continue;
        const double ang = std::atan2(vecs[i].y, vecs[i].x);
        s4 += std::sin(4 * ang);
        c4 += std::cos(4 * ang);
    }
    const double theta = std::atan2(s4, c4) / 4.0;
    const cv::Point2d ex(std::cos(theta), std::sin(theta));
    const cv::Point2d ey(-std::sin(theta), std::cos(theta));
    // all pair vectors with length ~ one pitch: mean projection along each axis
    double sumA = 0, sumB = 0;
    int nA = 0, nB = 0;
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j < n; ++j) {
            if (i == j) continue;
            const cv::Point2d d(pts[j].x - pts[i].x, pts[j].y - pts[i].y);
            const double len = std::hypot(d.x, d.y);
            if (len < 0.6 * p0 || len > 1.4 * p0) continue;
            const double px = d.dot(ex), py = d.dot(ey);
            if (std::abs(py) < 0.35 * p0 && std::abs(px) > 0.6 * p0) {
                sumA += std::abs(px);
                ++nA;
            }
            if (std::abs(px) < 0.35 * p0 && std::abs(py) > 0.6 * p0) {
                sumB += std::abs(py);
                ++nB;
            }
        }
    }
    const double pa = nA >= 4 ? sumA / nA : p0;
    const double pb = nB >= 4 ? sumB / nB : p0;
    aOut = ex * pa;
    bOut = ey * pb;
    return true;
}

struct LatticeFit {
    Affine M; // (u,v) lattice index -> pixel node position
    std::vector<cv::Point2i> idx;
    std::vector<cv::Point2i> dirs;
    double rms{0.0};
};

bool fitLattice(const std::vector<cv::Point2f>& pts, double dispRatio, LatticeFit& out) {
    cv::Point2d a, b;
    if (!estimateBasis(pts, a, b)) return false;
    const size_t n = pts.size();
    const cv::Point2f ref = pts[n / 2];
    Affine M;
    M.a = a.x;
    M.c = a.y;
    M.b = b.x;
    M.d = b.y;
    M.tx = ref.x;
    M.ty = ref.y;
    double ia, ib, ic, id;
    if (!M.invertLinear(ia, ib, ic, id)) return false;
    std::vector<cv::Point2i> idx(n), dirs(n, cv::Point2i(0, 0));
    for (size_t k = 0; k < n; ++k) {
        const double dx = pts[k].x - ref.x, dy = pts[k].y - ref.y;
        idx[k] = {static_cast<int>(std::lround(ia * dx + ib * dy)),
                  static_cast<int>(std::lround(ic * dx + id * dy))};
    }
    std::vector<cv::Point2d> uv(n), target(n);
    for (int it = 0; it < kFitIterations; ++it) {
        for (size_t k = 0; k < n; ++k) {
            uv[k] = {double(idx[k].x), double(idx[k].y)};
            // remove the currently assigned displacement (in lattice units) from the observation
            const double du = dirs[k].x * dispRatio, dv = dirs[k].y * dispRatio;
            target[k] = {pts[k].x - (M.a * du + M.b * dv), pts[k].y - (M.c * du + M.d * dv)};
        }
        if (!fitAffine(uv, target, M)) return false;
        if (!M.invertLinear(ia, ib, ic, id)) return false;
        for (size_t k = 0; k < n; ++k) {
            const auto node = M.apply(uv[k].x, uv[k].y);
            const double rx = pts[k].x - node.x, ry = pts[k].y - node.y;
            const double lu = (ia * rx + ib * ry) / dispRatio, lv = (ic * rx + id * ry) / dispRatio;
            if (std::abs(lu) >= std::abs(lv))
                dirs[k] = {lu >= 0 ? 1 : -1, 0};
            else
                dirs[k] = {0, lv >= 0 ? 1 : -1};
            const double px = pts[k].x - M.tx, py = pts[k].y - M.ty;
            idx[k] = {static_cast<int>(std::lround(ia * px + ib * py - dirs[k].x * dispRatio)),
                      static_cast<int>(std::lround(ic * px + id * py - dirs[k].y * dispRatio))};
        }
    }
    double sq = 0;
    for (size_t k = 0; k < n; ++k) {
        const auto node =
            M.apply(idx[k].x + dirs[k].x * dispRatio, idx[k].y + dirs[k].y * dispRatio);
        sq += (pts[k].x - node.x) * (pts[k].x - node.x) + (pts[k].y - node.y) * (pts[k].y - node.y);
    }
    out.M = M;
    out.idx = std::move(idx);
    out.dirs = std::move(dirs);
    out.rms = std::sqrt(sq / double(n));
    return true;
}

// Phase candidates for one line of bits (-1 = unknown).
std::vector<int> linePhases(const Codebook& cb, const std::vector<int8_t>& line) {
    std::vector<int> known;
    for (size_t s = 0; s < line.size(); ++s)
        if (line[s] >= 0) known.push_back(static_cast<int>(s));
    std::vector<int> cands;
    if (known.size() < static_cast<size_t>(kMnsOrder)) return cands;
    const auto& mns = cb.mns();
    for (int q = 0; q < kMnsPeriod; ++q) {
        bool okAll = true;
        for (int s : known) {
            if (mns[static_cast<size_t>((s + q) % kMnsPeriod)] != line[static_cast<size_t>(s)]) {
                okAll = false;
                break;
            }
        }
        if (okAll) {
            cands.push_back(q);
            if (cands.size() > static_cast<size_t>(kMaxPhaseCandidates)) {
                cands.clear();
                return cands;
            }
        }
    }
    return cands;
}

using VoteMap =
    std::map<std::pair<int, int>, int>; // (absolute index of line 0, cross phase) -> votes

VoteMap voteAxis(const std::vector<std::vector<int>>& phases, const std::vector<int>& table,
                 const Codebook& cb, bool columns) {
    VoteMap votes;
    const int nLines = static_cast<int>(phases.size());
    const int total = columns ? cb.params().columns : cb.params().rows;
    for (int k0 = 0; k0 + kWindowDots <= nLines; ++k0) {
        bool any = false;
        for (int i = 0; i < kWindowDots; ++i)
            if (phases[static_cast<size_t>(k0 + i)].empty()) {
                any = true;
                break;
            }
        if (any) continue;
        const auto& c0 = phases[static_cast<size_t>(k0)];
        const auto& c1 = phases[static_cast<size_t>(k0 + 1)];
        const auto& c2 = phases[static_cast<size_t>(k0 + 2)];
        for (int p0 : c0)
            for (int p1 : c1)
                for (int p2 : c2) {
                    const int d0 = (p1 - p0 + kMnsPeriod) % kMnsPeriod;
                    const int d1 = (p2 - p1 + kMnsPeriod) % kMnsPeriod;
                    const int K0 = columns ? cb.lookupColumn(d0, d1) : cb.lookupRow(d0, d1);
                    if (K0 < 0 || K0 + kWindowDots > total) continue;
                    const int cross0 =
                        (p0 - table[static_cast<size_t>(K0)] + kMnsPeriod) % kMnsPeriod;
                    if ((p1 - table[static_cast<size_t>(K0 + 1)] + kMnsPeriod) % kMnsPeriod !=
                        cross0)
                        continue;
                    if ((p2 - table[static_cast<size_t>(K0 + 2)] + kMnsPeriod) % kMnsPeriod !=
                        cross0)
                        continue;
                    ++votes[{K0 - k0, cross0}];
                }
    }
    return votes;
}

struct GridDecode {
    bool found{false};
    int transform{0};
    int I0{0}, J0{0};
    int votes{0};
    int runnerUp{0};
    int candidates{0};
    cv::Point2i tmin;
};

GridDecode decodeGrid(const Codebook& cb, const std::vector<cv::Point2i>& idx,
                      const std::vector<cv::Point2i>& dirs) {
    GridDecode best;
    const size_t n = idx.size();
    std::vector<cv::Point2i> ti(n), td(n);
    for (int t = 0; t < 8; ++t) {
        const auto& T = kDihedral[static_cast<size_t>(t)];
        cv::Point2i mn(INT_MAX, INT_MAX), mx(INT_MIN, INT_MIN);
        for (size_t k = 0; k < n; ++k) {
            ti[k] = {T[0] * idx[k].x + T[1] * idx[k].y, T[2] * idx[k].x + T[3] * idx[k].y};
            td[k] = {T[0] * dirs[k].x + T[1] * dirs[k].y, T[2] * dirs[k].x + T[3] * dirs[k].y};
            mn.x = std::min(mn.x, ti[k].x);
            mn.y = std::min(mn.y, ti[k].y);
            mx.x = std::max(mx.x, ti[k].x);
            mx.y = std::max(mx.y, ti[k].y);
        }
        const int W = mx.x - mn.x + 1, H = mx.y - mn.y + 1;
        if (W < kWindowDots || H < kMnsOrder) continue;
        std::vector<int8_t> xb(static_cast<size_t>(W * H), -1), yb(static_cast<size_t>(W * H), -1);
        for (size_t k = 0; k < n; ++k) {
            int xBit, yBit;
            if (!Codebook::bitsForDirection(td[k].x, td[k].y, xBit, yBit)) continue;
            const int u = ti[k].x - mn.x, v = ti[k].y - mn.y;
            xb[static_cast<size_t>(v * W + u)] = static_cast<int8_t>(xBit);
            yb[static_cast<size_t>(v * W + u)] = static_cast<int8_t>(yBit);
        }
        std::vector<std::vector<int>> colQ(static_cast<size_t>(W)), rowQ(static_cast<size_t>(H));
        std::vector<int8_t> line;
        for (int u = 0; u < W; ++u) {
            line.assign(static_cast<size_t>(H), -1);
            for (int v = 0; v < H; ++v)
                line[static_cast<size_t>(v)] = xb[static_cast<size_t>(v * W + u)];
            colQ[static_cast<size_t>(u)] = linePhases(cb, line);
        }
        for (int v = 0; v < H; ++v) {
            line.assign(static_cast<size_t>(W), -1);
            for (int u = 0; u < W; ++u)
                line[static_cast<size_t>(u)] = yb[static_cast<size_t>(v * W + u)];
            rowQ[static_cast<size_t>(v)] = linePhases(cb, line);
        }
        const VoteMap colVotes = voteAxis(colQ, cb.phi(), cb, true);  // (I0, J0 mod 63)
        const VoteMap rowVotes = voteAxis(rowQ, cb.psi(), cb, false); // (J0, I0 mod 63)
        VoteMap combined;
        for (const auto& [ck, nc] : colVotes)
            for (const auto& [rk, nr] : rowVotes) {
                const int I0 = ck.first, j0m = ck.second, J0 = rk.first, i0m = rk.second;
                if (((I0 % kMnsPeriod) + kMnsPeriod) % kMnsPeriod != i0m) continue;
                if (((J0 % kMnsPeriod) + kMnsPeriod) % kMnsPeriod != j0m) continue;
                if (I0 < 0 || I0 >= cb.params().columns || J0 < 0 || J0 >= cb.params().rows)
                    continue;
                combined[{I0, J0}] += nc + nr;
            }
        for (const auto& [key, votes] : combined) {
            best.candidates += votes;
            if (!best.found || votes > best.votes) {
                if (best.found) best.runnerUp = std::max(best.runnerUp, best.votes);
                best.found = true;
                best.transform = t;
                best.I0 = key.first;
                best.J0 = key.second;
                best.votes = votes;
                best.tmin = mn;
            } else {
                best.runnerUp = std::max(best.runnerUp, votes);
            }
        }
    }
    return best;
}

} // namespace

// ------------------------------------------------------------------ Decoder

Decoder::Decoder(std::shared_ptr<const Codebook> codebook) : codebook_(std::move(codebook)) {}

std::vector<cv::Point2f> Decoder::detectDots(const cv::Mat& input, double expectedDiameterPx) {
    cv::Mat gray;
    if (input.channels() == 3)
        cv::cvtColor(input, gray, cv::COLOR_BGR2GRAY);
    else
        gray = input;
    if (gray.depth() != CV_8U) cv::normalize(gray, gray, 0, 255, cv::NORM_MINMAX, CV_8U);
    std::vector<cv::Point2f> out;
    if (gray.empty() || expectedDiameterPx <= 1.0) return out;
    int k = static_cast<int>(std::max(3.0, expectedDiameterPx * 6.0)) | 1;
    cv::Mat bg, diff, binary;
    cv::GaussianBlur(gray, bg, cv::Size(k, k), 0);
    cv::subtract(bg, gray, diff); // dark dots -> positive
    double maxVal = 0;
    cv::minMaxLoc(diff, nullptr, &maxVal);
    const int thr = std::max(8, static_cast<int>(maxVal * 0.4));
    cv::threshold(diff, binary, thr, 255, cv::THRESH_BINARY);
    cv::Mat labels, stats, cents;
    const int n = cv::connectedComponentsWithStats(binary, labels, stats, cents, 8, CV_32S);
    const double areaExpected = CV_PI * (expectedDiameterPx / 2) * (expectedDiameterPx / 2);
    for (int i = 1; i < n; ++i) {
        const int a = stats.at<int>(i, cv::CC_STAT_AREA);
        const int w = stats.at<int>(i, cv::CC_STAT_WIDTH), h = stats.at<int>(i, cv::CC_STAT_HEIGHT);
        if (a < areaExpected * 0.3 || a > areaExpected * 3.0) continue;
        if (std::max(w, h) > 1.8 * std::min(w, h)) continue;
        if (double(a) / double(w * h) < 0.5) continue;
        const int x0 = stats.at<int>(i, cv::CC_STAT_LEFT), y0 = stats.at<int>(i, cv::CC_STAT_TOP);
        if (x0 <= 0 || y0 <= 0 || x0 + w >= binary.cols || y0 + h >= binary.rows)
            continue; // clipped
        out.emplace_back(static_cast<float>(cents.at<double>(i, 0)),
                         static_cast<float>(cents.at<double>(i, 1)));
    }
    return out;
}

DecodeResult Decoder::decode(const cv::Mat& gray, const DecoderConfig& config) const {
    const auto t0 = std::chrono::steady_clock::now();
    DecodeResult r;
    auto finish = [&](DecodeResult& res) -> DecodeResult& {
        res.decodeMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                .count();
        return res;
    };
    if (!codebook_ || !codebook_->valid()) {
        r.reason = "no codebook";
        return finish(r);
    }
    if (gray.empty()) {
        r.reason = "empty image";
        return finish(r);
    }
    const auto& P = codebook_->params();
    const double diamPx = P.dotDiameterUm / std::max(1e-6, config.umPerPxHint);
    std::vector<cv::Point2f> pts = detectDots(gray, diamPx);
    r.dots = static_cast<int>(pts.size());
    if (pts.size() < static_cast<size_t>(kWindowDots * kMnsOrder)) {
        r.reason = "too few dots";
        return finish(r);
    }
    if (pts.size() > static_cast<size_t>(config.maxDots)) {
        r.reason = "too many blobs";
        return finish(r);
    }
    const double dispRatio = P.displacementUm / P.pitchUm;
    LatticeFit fit;
    if (!fitLattice(pts, dispRatio, fit)) {
        r.reason = "lattice fit failed";
        return finish(r);
    }
    r.residualPx = fit.rms;
    r.dotsPx = pts;
    const GridDecode g = decodeGrid(*codebook_, fit.idx, fit.dirs);
    r.candidates = g.candidates;
    r.votes = g.votes;
    if (!g.found || g.votes < config.minVotes) {
        r.reason = "no consistent code window";
        return finish(r);
    }
    if (g.runnerUp * 2 > g.votes) {
        r.reason = "ambiguous code windows";
        return finish(r);
    }
    const auto& T = kDihedral[static_cast<size_t>(g.transform)];
    std::vector<cv::Point2d> nodePx, nodeUm;
    int agree = 0;
    const size_t n = pts.size();
    for (size_t k = 0; k < n; ++k) {
        const int I = T[0] * fit.idx[k].x + T[1] * fit.idx[k].y - g.tmin.x + g.I0;
        const int J = T[2] * fit.idx[k].x + T[3] * fit.idx[k].y - g.tmin.y + g.J0;
        const int dx = T[0] * fit.dirs[k].x + T[1] * fit.dirs[k].y;
        const int dy = T[2] * fit.dirs[k].x + T[3] * fit.dirs[k].y;
        if (I < 0 || I >= P.columns || J < 0 || J >= P.rows) continue;
        if (codebook_->direction(I, J) != std::make_pair(dx, dy)) continue;
        ++agree;
        nodePx.push_back(fit.M.apply(fit.idx[k].x, fit.idx[k].y));
        const auto um = codebook_->nodeUm(I, J);
        nodeUm.emplace_back(um.first, um.second);
    }
    r.agreement = double(agree) / double(n);
    if (r.agreement < config.minAgreement || agree < config.minAgreeingDots) {
        r.reason = "bit agreement too low";
        return finish(r);
    }
    Affine Pw;
    if (!fitAffine(nodePx, nodeUm, Pw)) {
        r.reason = "pose fit failed";
        return finish(r);
    }
    r.pixelToWafer[0] = Pw.a;
    r.pixelToWafer[1] = Pw.b;
    r.pixelToWafer[2] = Pw.tx;
    r.pixelToWafer[3] = Pw.c;
    r.pixelToWafer[4] = Pw.d;
    r.pixelToWafer[5] = Pw.ty;
    r.mirrored = Pw.det() < 0;
    r.umPerPx = std::hypot(Pw.a, Pw.c);
    const double sgn = r.mirrored ? -1.0 : 1.0;
    r.thetaDeg = std::atan2(sgn * Pw.c, sgn * Pw.a) * 180.0 / CV_PI;
    const auto centre = Pw.apply(gray.cols / 2.0, gray.rows / 2.0);
    r.centreXUm = centre.x;
    r.centreYUm = centre.y;
    if (const Chip* chip = codebook_->chipAt(centre.x, centre.y)) r.chip = chip->name;
    r.ok = true;
    return finish(r);
}

// ------------------------------------------------------------------ synthetic view

void ViewPose::matrix(double m[6]) const {
    const double t = thetaDeg * CV_PI / 180.0;
    const double c = std::cos(t), s = std::sin(t);
    const double sx = (mirrored ? -1.0 : 1.0) * umPerPx, sy = umPerPx;
    // A = R * diag(sx, sy)
    m[0] = c * sx;
    m[1] = -s * sy;
    m[3] = s * sx;
    m[4] = c * sy;
    const double cu = width / 2.0, cv = height / 2.0;
    m[2] = centreXUm - (m[0] * cu + m[1] * cv);
    m[5] = centreYUm - (m[3] * cu + m[4] * cv);
}

void ViewPose::inverse(double inv[6]) const {
    double m[6];
    matrix(m);
    const double det = m[0] * m[4] - m[1] * m[3];
    inv[0] = m[4] / det;
    inv[1] = -m[1] / det;
    inv[3] = -m[3] / det;
    inv[4] = m[0] / det;
    inv[2] = -(inv[0] * m[2] + inv[1] * m[5]);
    inv[5] = -(inv[3] * m[2] + inv[4] * m[5]);
}

cv::Mat renderView(const Codebook& cb, const ViewPose& pose, const RenderOptions& opt) {
    cv::Mat img(pose.height, pose.width, CV_8UC1, cv::Scalar(opt.background));
    double m[6], inv[6];
    pose.matrix(m);
    pose.inverse(inv);
    const auto& P = cb.params();
    double xmin = 1e300, xmax = -1e300, ymin = 1e300, ymax = -1e300;
    for (const auto& c : {cv::Point2d(0, 0), cv::Point2d(pose.width, 0),
                          cv::Point2d(0, pose.height), cv::Point2d(pose.width, pose.height)}) {
        const double x = m[0] * c.x + m[1] * c.y + m[2], y = m[3] * c.x + m[4] * c.y + m[5];
        xmin = std::min(xmin, x);
        xmax = std::max(xmax, x);
        ymin = std::min(ymin, y);
        ymax = std::max(ymax, y);
    }
    const int i0 = std::max(0, static_cast<int>(std::floor((xmin - P.originXUm) / P.pitchUm)) - 1);
    const int i1 =
        std::min(P.columns - 1, static_cast<int>(std::ceil((xmax - P.originXUm) / P.pitchUm)) + 1);
    const int j0 = std::max(0, static_cast<int>(std::floor((ymin - P.originYUm) / P.pitchUm)) - 1);
    const int j1 =
        std::min(P.rows - 1, static_cast<int>(std::ceil((ymax - P.originYUm) / P.pitchUm)) + 1);
    const double rPx = P.dotDiameterUm / 2.0 / pose.umPerPx;
    std::mt19937 rng(opt.seed);
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    // channel band geometry
    double dx = 0, dy = 0, len = 0;
    if (opt.channel) {
        dx = opt.channelX1 - opt.channelX0;
        dy = opt.channelY1 - opt.channelY0;
        len = std::hypot(dx, dy);
        if (len > 0) {
            dx /= len;
            dy /= len;
        }
    }
    for (int i = i0; i <= i1; ++i)
        for (int j = j0; j <= j1; ++j) {
            if (opt.missingFraction > 0 && uni(rng) < opt.missingFraction) continue;
            const auto d = cb.dotUm(i, j);
            if (opt.channel && len > 0) {
                const double perp =
                    std::abs(-(d.first - opt.channelX0) * dy + (d.second - opt.channelY0) * dx);
                if (perp < opt.bandUm) continue;
            }
            const double u = inv[0] * d.first + inv[1] * d.second + inv[2];
            const double v = inv[3] * d.first + inv[4] * d.second + inv[5];
            if (u < -rPx || u >= pose.width + rPx || v < -rPx || v >= pose.height + rPx) continue;
            cv::circle(img,
                       cv::Point(static_cast<int>(std::lround(u * 16)),
                                 static_cast<int>(std::lround(v * 16))),
                       static_cast<int>(std::lround(rPx * 16)), cv::Scalar(opt.dotLevel), -1,
                       cv::LINE_AA, 4);
        }
    if (opt.channel) {
        const double u0 = inv[0] * opt.channelX0 + inv[1] * opt.channelY0 + inv[2];
        const double v0 = inv[3] * opt.channelX0 + inv[4] * opt.channelY0 + inv[5];
        const double u1 = inv[0] * opt.channelX1 + inv[1] * opt.channelY1 + inv[2];
        const double v1 = inv[3] * opt.channelX1 + inv[4] * opt.channelY1 + inv[5];
        const int w = std::max(1, static_cast<int>(std::lround(opt.channelWidthUm / pose.umPerPx)));
        cv::line(img, cv::Point(int(u0), int(v0)), cv::Point(int(u1), int(v1)),
                 cv::Scalar(opt.dotLevel + 40), w, cv::LINE_AA);
    }
    if (opt.blurSigmaPx > 0) cv::GaussianBlur(img, img, cv::Size(0, 0), opt.blurSigmaPx);
    if (opt.noiseSigma > 0) {
        cv::Mat noise(img.size(), CV_32F);
        cv::RNG cvrng(opt.seed + 1);
        cvrng.fill(noise, cv::RNG::NORMAL, 0.0, opt.noiseSigma);
        cv::Mat f;
        img.convertTo(f, CV_32F);
        f += noise;
        f.convertTo(img, CV_8U);
    }
    return img;
}

} // namespace backend::dotgrid
