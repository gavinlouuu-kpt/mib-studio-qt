// F-4 (#808 F09, I10): native inference and concentration-response fits.
// See include/backend/analysis/AnalysisInference.h. Every algorithm here is a
// textbook one; reference values come from scipy via
// tools/analysis_inference/generate_reference.py.
#include "backend/analysis/AnalysisInference.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <stdexcept>

namespace backend::analysis::inference {
namespace {
constexpr double kEps = std::numeric_limits<double>::epsilon();
constexpr double kTiny = 1e-300;
constexpr double kLn10 = 2.302585092994045684;

double betaContinuedFraction(double a, double b, double x) {
    const double qab = a + b, qap = a + 1, qam = a - 1;
    double c = 1, d = 1 - qab * x / qap;
    if (std::fabs(d) < kTiny) d = kTiny;
    d = 1 / d;
    double h = d;
    for (int m = 1; m <= 10000; ++m) {
        const int m2 = 2 * m;
        double aa = m * (b - m) * x / ((qam + m2) * (a + m2));
        d = 1 + aa * d;
        if (std::fabs(d) < kTiny) d = kTiny;
        c = 1 + aa / c;
        if (std::fabs(c) < kTiny) c = kTiny;
        d = 1 / d;
        h *= d * c;
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2));
        d = 1 + aa * d;
        if (std::fabs(d) < kTiny) d = kTiny;
        c = 1 + aa / c;
        if (std::fabs(c) < kTiny) c = kTiny;
        d = 1 / d;
        const double del = d * c;
        h *= del;
        if (std::fabs(del - 1) < 4 * kEps) return h;
    }
    return h;
}

// Upper regularized gamma Q(a, x) by continued fraction (x >= a + 1).
double gammaContinuedFraction(double a, double x) {
    double b = x + 1 - a, c = 1 / kTiny, d = 1 / b, h = d;
    for (int i = 1; i <= 10000; ++i) {
        const double an = -i * (i - a);
        b += 2;
        d = an * d + b;
        if (std::fabs(d) < kTiny) d = kTiny;
        c = b + an / c;
        if (std::fabs(c) < kTiny) c = kTiny;
        d = 1 / d;
        const double del = d * c;
        h *= del;
        if (std::fabs(del - 1) < 4 * kEps) break;
    }
    return std::exp(-x + a * std::log(x) - std::lgamma(a)) * h;
}
// Lower regularized gamma P(a, x) by series (x < a + 1).
double gammaSeries(double a, double x) {
    double ap = a, sum = 1 / a, del = sum;
    for (int n = 0; n < 100000; ++n) {
        ap += 1;
        del *= x / ap;
        sum += del;
        if (std::fabs(del) < std::fabs(sum) * 4 * kEps) break;
    }
    return sum * std::exp(-x + a * std::log(x) - std::lgamma(a));
}
double gammaUpper(double a, double x) {
    if (x <= 0) return 1;
    return x < a + 1 ? 1 - gammaSeries(a, x) : gammaContinuedFraction(a, x);
}

// Two-sided tail of Student t: P(|T| >= |t|) = I_{df/(df+t^2)}(df/2, 1/2).
double studentTTwoSided(double t, double df) {
    if (!std::isfinite(t)) return 0;
    return regularizedIncompleteBeta(df / 2, 0.5, df / (df + t * t));
}

double mean(const std::vector<double>& v) {
    long double s = 0;
    for (double x : v) s += x;
    return static_cast<double>(s / v.size());
}
double sampleVariance(const std::vector<double>& v) {
    const double m = mean(v);
    long double s = 0;
    for (double x : v) s += (x - m) * static_cast<long double>(x - m);
    return static_cast<double>(s / (v.size() - 1));
}
double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    const auto n = v.size();
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

// Average ranks (1-based) of `values`, with the tie-group sizes.
std::vector<double> averageRanks(const std::vector<double>& values, std::vector<double>& ties) {
    std::vector<std::size_t> order(values.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(),
              [&](std::size_t a, std::size_t b) { return values[a] < values[b]; });
    std::vector<double> ranks(values.size());
    ties.clear();
    for (std::size_t i = 0; i < order.size();) {
        std::size_t j = i;
        while (j + 1 < order.size() && values[order[j + 1]] == values[order[i]]) ++j;
        const double rank = (i + j + 2) / 2.0;
        for (std::size_t k = i; k <= j; ++k) ranks[order[k]] = rank;
        if (j > i) ties.push_back(static_cast<double>(j - i + 1));
        i = j + 1;
    }
    return ranks;
}
double tieSum(const std::vector<double>& ties) {
    double s = 0;
    for (double t : ties) s += t * t * t - t;
    return s;
}

void requireFinite(const std::vector<double>& v, std::size_t minimum, const char* what) {
    if (v.size() < minimum) throw std::invalid_argument(std::string(what) + ": too few values");
    for (double x : v)
        if (!std::isfinite(x)) throw std::invalid_argument(std::string(what) + ": nonfinite value");
}

// Exact null distribution of the Mann-Whitney U (no ties): P(U >= u).
double mannWhitneyExactUpper(std::size_t m, std::size_t n, std::size_t u) {
    // p(u; i, j) = i/(i+j) p(u - j; i-1, j) + j/(i+j) p(u; i, j-1), as probabilities.
    const std::size_t maxU = m * n;
    std::vector<std::vector<double>> previous(n + 1), current(n + 1);
    for (std::size_t j = 0; j <= n; ++j) previous[j] = {1.0}; // i = 0: U = 0 surely
    for (std::size_t i = 1; i <= m; ++i) {
        current[0] = {1.0};
        for (std::size_t j = 1; j <= n; ++j) {
            std::vector<double> p(i * j + 1, 0.0);
            const double a = static_cast<double>(i) / (i + j), b = static_cast<double>(j) / (i + j);
            for (std::size_t k = 0; k < previous[j].size(); ++k) p[k + j] += a * previous[j][k];
            for (std::size_t k = 0; k < current[j - 1].size(); ++k) p[k] += b * current[j - 1][k];
            current[j] = std::move(p);
        }
        std::swap(previous, current);
    }
    const auto& dist = previous[n];
    double tail = 0;
    for (std::size_t k = u; k <= maxU && k < dist.size(); ++k) tail += dist[k];
    return tail;
}
// Exact null distribution of the Wilcoxon signed-rank W+ (no ties/zeros): P(W <= w).
double wilcoxonExactLower(std::size_t n, std::size_t w) {
    const std::size_t maxSum = n * (n + 1) / 2;
    std::vector<double> p(maxSum + 1, 0.0), next(maxSum + 1, 0.0);
    p[0] = 1;
    for (std::size_t k = 1; k <= n; ++k) {
        for (std::size_t s = 0; s <= maxSum; ++s)
            next[s] = 0.5 * p[s] + (s >= k ? 0.5 * p[s - k] : 0.0);
        std::swap(p, next);
    }
    double cdf = 0;
    for (std::size_t s = 0; s <= w && s < p.size(); ++s) cdf += p[s];
    return cdf;
}
} // namespace

// ---- distributions -------------------------------------------------------
double regularizedIncompleteBeta(double a, double b, double x) {
    if (!(a > 0) || !(b > 0) || std::isnan(x)) return std::numeric_limits<double>::quiet_NaN();
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    const double front = std::exp(std::lgamma(a + b) - std::lgamma(a) - std::lgamma(b) +
                                  a * std::log(x) + b * std::log1p(-x));
    if (x < (a + 1) / (a + b + 2)) return front * betaContinuedFraction(a, b, x) / a;
    return 1 - front * betaContinuedFraction(b, a, 1 - x) / b;
}
double regularizedLowerGamma(double a, double x) {
    if (x <= 0) return 0;
    return x < a + 1 ? gammaSeries(a, x) : 1 - gammaContinuedFraction(a, x);
}
double normalCdf(double z) { return 0.5 * std::erfc(-z / std::sqrt(2.0)); }
double normalQuantile(double p) {
    if (!(p > 0) || !(p < 1)) return p == 0 ? -INFINITY : p == 1 ? INFINITY : NAN;
    static const double a[] = {-3.969683028665376e+01, 2.209460984245205e+02,
                               -2.759285104469687e+02, 1.383577518672690e+02,
                               -3.066479806614716e+01, 2.506628277459239e+00};
    static const double b[] = {-5.447609879822406e+01, 1.615858368580409e+02,
                               -1.556989798598866e+02, 6.680131188771972e+01,
                               -1.328068155288572e+01};
    static const double c[] = {-7.784894002430293e-03, -3.223964580411365e-01,
                               -2.400758277161838e+00, -2.549732539343734e+00,
                               4.374664141464968e+00,  2.938163982698783e+00};
    static const double d[] = {7.784695709041462e-03, 3.224671290700398e-01,
                               2.445134137142996e+00, 3.754408661907416e+00};
    double x;
    if (p < 0.02425) {
        const double q = std::sqrt(-2 * std::log(p));
        x = (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
            ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1);
    } else if (p > 1 - 0.02425) {
        const double q = std::sqrt(-2 * std::log(1 - p));
        x = -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
            ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1);
    } else {
        const double q = p - 0.5, r = q * q;
        x = (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q /
            (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1);
    }
    const double e = normalCdf(x) - p;
    const double u = e * std::sqrt(2 * 3.14159265358979323846) * std::exp(x * x / 2);
    return x - u / (1 + x * u / 2);
}
double studentTCdf(double t, double df) {
    if (std::isnan(t) || !(df > 0)) return NAN;
    const double tail = 0.5 * studentTTwoSided(t, df);
    return t > 0 ? 1 - tail : tail;
}
double studentTQuantile(double p, double df) {
    if (!(p > 0) || !(p < 1) || !(df > 0)) return NAN;
    if (p == 0.5) return 0;
    if (p < 0.5) return -studentTQuantile(1 - p, df);
    const double tail = 2 * (1 - p); // two-sided tail to match
    double lo = 0, hi = 1;
    while (studentTTwoSided(hi, df) > tail && hi < 1e300) hi *= 2;
    for (int i = 0; i < 400 && hi - lo > 1e-15 * hi; ++i) {
        const double mid = 0.5 * (lo + hi);
        (studentTTwoSided(mid, df) > tail ? lo : hi) = mid;
    }
    return 0.5 * (lo + hi);
}
double fSurvival(double f, double d1, double d2) {
    if (!(f > 0)) return 1;
    if (!std::isfinite(f)) return 0;
    return regularizedIncompleteBeta(d2 / 2, d1 / 2, d2 / (d2 + d1 * f));
}
double chiSquareSurvival(double x, double k) { return gammaUpper(k / 2, x / 2); }

// ---- tests ---------------------------------------------------------------
TestResult welchT(const std::vector<double>& reference, const std::vector<double>& comparator,
                  double confidenceLevel) {
    requireFinite(reference, 2, "Welch t reference");
    requireFinite(comparator, 2, "Welch t comparator");
    const double na = reference.size(), nb = comparator.size();
    const double va = sampleVariance(reference), vb = sampleVariance(comparator);
    const double diff = mean(comparator) - mean(reference);
    const double sa = va / na, sb = vb / nb, se = std::sqrt(sa + sb);
    TestResult r;
    r.test = "welch-t";
    r.statisticName = "t";
    r.estimateName = "difference of means (comparator - reference)";
    r.estimate = diff;
    r.effectSizeName = "Hedges g (pooled SD, small-sample corrected)";
    const double pooled = std::sqrt(((na - 1) * va + (nb - 1) * vb) / (na + nb - 2));
    if (pooled > 0) r.effectSize = diff / pooled * (1 - 3 / (4 * (na + nb) - 9));
    if (!(se > 0)) {
        r.statistic = 0;
        r.pValue = 1;
        r.pMethod = "undefined: zero variance in both groups";
        r.ciUnavailableReason = "zero standard error";
        r.notes.push_back("both groups have zero variance; no test statistic");
        return r;
    }
    const double df = (sa + sb) * (sa + sb) / (sa * sa / (na - 1) + sb * sb / (nb - 1));
    r.statistic = diff / se;
    r.df1 = df;
    r.pValue = studentTTwoSided(r.statistic, df);
    r.pMethod = "two-sided Student t with Welch-Satterthwaite df (regularized incomplete beta)";
    const double q = studentTQuantile(0.5 + confidenceLevel / 2, df);
    r.confidenceInterval = Interval{diff - q * se, diff + q * se};
    return r;
}

TestResult mannWhitneyU(const std::vector<double>& reference,
                        const std::vector<double>& comparator) {
    requireFinite(reference, 1, "Mann-Whitney reference");
    requireFinite(comparator, 1, "Mann-Whitney comparator");
    const std::size_t n1 = comparator.size(), n2 = reference.size(), n = n1 + n2;
    std::vector<double> all(comparator);
    all.insert(all.end(), reference.begin(), reference.end());
    std::vector<double> ties;
    const auto ranks = averageRanks(all, ties);
    double r1 = 0;
    for (std::size_t i = 0; i < n1; ++i) r1 += ranks[i];
    const double u1 = r1 - n1 * (n1 + 1) / 2.0, u2 = double(n1) * n2 - u1;
    TestResult r;
    r.test = "mann-whitney-u";
    r.statisticName = "U";
    r.statistic = u1;
    const double big = std::max(u1, u2);
    if (ties.empty() && n1 <= 50 && n2 <= 50) {
        r.pValue = std::min(1.0, 2 * mannWhitneyExactUpper(n1, n2, static_cast<std::size_t>(
                                                                       std::llround(big))));
        r.pMethod = "exact null distribution (no ties, both n <= 50), two-sided";
    } else {
        const double mu = n1 * double(n2) / 2;
        const double s = std::sqrt(n1 * double(n2) / 12.0 *
                                   ((n + 1) - tieSum(ties) / (double(n) * (n - 1))));
        const double z = s > 0 ? (big - mu - 0.5) / s : 0;
        r.pValue = std::min(1.0, 2 * normalCdf(-z));
        r.pMethod = "normal approximation with tie correction and continuity correction 0.5, "
                    "two-sided";
    }
    std::vector<double> shifts;
    shifts.reserve(n1 * n2);
    for (double b : comparator)
        for (double a : reference) shifts.push_back(b - a);
    r.estimateName = "Hodges-Lehmann shift (median of comparator - reference differences)";
    r.estimate = median(shifts);
    r.ciUnavailableReason = "no confidence interval is computed for the rank shift";
    r.effectSizeName = "rank-biserial correlation 2U/(n1 n2) - 1";
    r.effectSize = 2 * u1 / (double(n1) * n2) - 1;
    return r;
}

TestResult pairedT(const std::vector<double>& reference, const std::vector<double>& comparator,
                   double confidenceLevel) {
    if (reference.size() != comparator.size())
        throw std::invalid_argument("Paired t requires matched units");
    requireFinite(reference, 2, "Paired t");
    requireFinite(comparator, 2, "Paired t");
    std::vector<double> d(reference.size());
    for (std::size_t i = 0; i < d.size(); ++i) d[i] = comparator[i] - reference[i];
    const double n = d.size(), m = mean(d), sd = std::sqrt(sampleVariance(d)), se = sd / std::sqrt(n);
    TestResult r;
    r.test = "paired-t";
    r.statisticName = "t";
    r.estimateName = "mean paired difference (comparator - reference)";
    r.estimate = m;
    r.effectSizeName = "Cohen dz (mean difference / SD of differences)";
    r.df1 = n - 1;
    if (!(se > 0)) {
        r.pValue = 1;
        r.pMethod = "undefined: zero variance of differences";
        r.ciUnavailableReason = "zero standard error";
        return r;
    }
    r.effectSize = m / sd;
    r.statistic = m / se;
    r.pValue = studentTTwoSided(r.statistic, n - 1);
    r.pMethod = "two-sided Student t, df = n - 1 (regularized incomplete beta)";
    const double q = studentTQuantile(0.5 + confidenceLevel / 2, n - 1);
    r.confidenceInterval = Interval{m - q * se, m + q * se};
    return r;
}

TestResult wilcoxonSignedRank(const std::vector<double>& reference,
                              const std::vector<double>& comparator) {
    if (reference.size() != comparator.size())
        throw std::invalid_argument("Wilcoxon signed-rank requires matched units");
    requireFinite(reference, 1, "Wilcoxon");
    requireFinite(comparator, 1, "Wilcoxon");
    std::vector<double> d, all;
    for (std::size_t i = 0; i < reference.size(); ++i) {
        const double v = comparator[i] - reference[i];
        all.push_back(v);
        if (v != 0) d.push_back(v);
    }
    TestResult r;
    r.test = "wilcoxon-signed-rank";
    r.statisticName = "W";
    const std::size_t zeros = all.size() - d.size();
    if (zeros) r.notes.push_back(std::to_string(zeros) + " zero difference(s) dropped (Wilcoxon)");
    std::vector<double> walsh;
    for (std::size_t i = 0; i < all.size(); ++i)
        for (std::size_t j = i; j < all.size(); ++j) walsh.push_back((all[i] + all[j]) / 2);
    r.estimateName = "Hodges-Lehmann pseudo-median of paired differences";
    r.estimate = median(walsh);
    r.ciUnavailableReason = "no confidence interval is computed for the pseudo-median";
    r.effectSizeName = "matched-pairs rank-biserial (W+ - W-) / (W+ + W-)";
    if (d.empty()) {
        r.pValue = 1;
        r.pMethod = "undefined: every paired difference is zero";
        return r;
    }
    std::vector<double> magnitude(d.size()), ties;
    for (std::size_t i = 0; i < d.size(); ++i) magnitude[i] = std::fabs(d[i]);
    const auto ranks = averageRanks(magnitude, ties);
    double plus = 0, minus = 0;
    for (std::size_t i = 0; i < d.size(); ++i) (d[i] > 0 ? plus : minus) += ranks[i];
    const double n = d.size();
    r.statistic = std::min(plus, minus);
    r.effectSize = (plus - minus) / (plus + minus);
    if (ties.empty() && d.size() <= 50) {
        r.pValue = std::min(
            1.0, 2 * wilcoxonExactLower(d.size(), static_cast<std::size_t>(std::llround(r.statistic))));
        r.pMethod = "exact null distribution (no ties, n <= 50 after dropping zeros), two-sided";
    } else {
        const double mn = n * (n + 1) / 4;
        const double se = std::sqrt((n * (n + 1) * (2 * n + 1) - 0.5 * tieSum(ties)) / 24);
        const double z = se > 0 ? (r.statistic - mn) / se : 0;
        r.pValue = std::min(1.0, 2 * normalCdf(-std::fabs(z)));
        r.pMethod = "normal approximation with tie correction, no continuity correction, two-sided";
    }
    return r;
}

TestResult oneWayAnova(const std::vector<std::vector<double>>& groups) {
    if (groups.size() < 2) throw std::invalid_argument("ANOVA requires two or more groups");
    std::vector<double> all;
    for (const auto& g : groups) {
        requireFinite(g, 1, "ANOVA");
        all.insert(all.end(), g.begin(), g.end());
    }
    const double grand = mean(all), k = groups.size(), n = all.size();
    long double between = 0, within = 0;
    for (const auto& g : groups) {
        const double m = mean(g);
        between += g.size() * static_cast<long double>(m - grand) * (m - grand);
        for (double x : g) within += static_cast<long double>(x - m) * (x - m);
    }
    TestResult r;
    r.test = "one-way-anova";
    r.statisticName = "F";
    r.df1 = k - 1;
    r.df2 = n - k;
    const double msb = static_cast<double>(between) / (k - 1);
    const double msw = static_cast<double>(within) / (n - k);
    r.effectSizeName = "eta squared SSB / SST";
    if (between + within > 0) r.effectSize = static_cast<double>(between / (between + within));
    r.estimateName = "";
    r.ciUnavailableReason = "omnibus test; no single contrast estimate";
    if (!(msw > 0)) {
        r.pValue = between > 0 ? 0 : 1;
        r.statistic = between > 0 ? INFINITY : 0;
        r.pMethod = "degenerate: zero within-group variance";
        return r;
    }
    r.statistic = msb / msw;
    r.pValue = fSurvival(r.statistic, k - 1, n - k);
    r.pMethod = "upper tail of F(k-1, N-k) (regularized incomplete beta)";
    return r;
}

TestResult kruskalWallis(const std::vector<std::vector<double>>& groups) {
    if (groups.size() < 2) throw std::invalid_argument("Kruskal-Wallis requires two or more groups");
    std::vector<double> all;
    for (const auto& g : groups) {
        requireFinite(g, 1, "Kruskal-Wallis");
        all.insert(all.end(), g.begin(), g.end());
    }
    std::vector<double> ties;
    const auto ranks = averageRanks(all, ties);
    const double n = all.size(), k = groups.size();
    double sum = 0;
    std::size_t offset = 0;
    for (const auto& g : groups) {
        double rs = 0;
        for (std::size_t i = 0; i < g.size(); ++i) rs += ranks[offset + i];
        offset += g.size();
        sum += rs * rs / g.size();
    }
    double h = 12 / (n * (n + 1)) * sum - 3 * (n + 1);
    const double correction = 1 - tieSum(ties) / (n * n * n - n);
    TestResult r;
    r.test = "kruskal-wallis";
    r.statisticName = "H";
    r.df1 = k - 1;
    r.ciUnavailableReason = "omnibus test; no single contrast estimate";
    r.effectSizeName = "epsilon squared H (N + 1) / (N^2 - 1)";
    if (!(correction > 0)) {
        r.pValue = 1;
        r.pMethod = "undefined: every value tied";
        return r;
    }
    h /= correction;
    r.statistic = h;
    r.effectSize = h * (n + 1) / (n * n - 1);
    r.pValue = chiSquareSurvival(h, k - 1);
    r.pMethod = "chi-square approximation with k-1 df, tie-corrected H (regularized gamma)";
    return r;
}

// ---- concentration-response fit -------------------------------------------
namespace {
struct Logistic {
    bool fixedHill;
    std::size_t parameters() const { return fixedHill ? 3 : 4; }
    double hill(const std::vector<double>& t) const { return fixedHill ? 1.0 : t[3]; }
    // value and gradient at x = log10 concentration
    double eval(double x, const std::vector<double>& t, double* grad) const {
        const double b = t[0], top = t[1], c = t[2], h = hill(t);
        const double e = std::clamp((c - x) * h, -300.0, 300.0);
        const double u = std::pow(10.0, e), d = 1 + u;
        if (grad) {
            const double uOverD2 = (u / d) / d;
            grad[0] = u / d;
            grad[1] = 1 / d;
            grad[2] = -(top - b) * uOverD2 * kLn10 * h;
            if (!fixedHill) grad[3] = -(top - b) * uOverD2 * kLn10 * (c - x);
        }
        return b + (top - b) / d;
    }
};

// Solve M x = v (p <= 4) by Gaussian elimination with partial pivoting.
bool solve(std::vector<std::vector<double>> m, std::vector<double> v, std::vector<double>& x) {
    const std::size_t p = v.size();
    for (std::size_t col = 0; col < p; ++col) {
        std::size_t pivot = col;
        for (std::size_t r = col + 1; r < p; ++r)
            if (std::fabs(m[r][col]) > std::fabs(m[pivot][col])) pivot = r;
        if (!(std::fabs(m[pivot][col]) > 0)) return false;
        std::swap(m[pivot], m[col]);
        std::swap(v[pivot], v[col]);
        for (std::size_t r = col + 1; r < p; ++r) {
            const double f = m[r][col] / m[col][col];
            for (std::size_t c = col; c < p; ++c) m[r][c] -= f * m[col][c];
            v[r] -= f * v[col];
        }
    }
    x.assign(p, 0);
    for (std::size_t r = p; r-- > 0;) {
        double s = v[r];
        for (std::size_t c = r + 1; c < p; ++c) s -= m[r][c] * x[c];
        x[r] = s / m[r][r];
    }
    for (double value : x)
        if (!std::isfinite(value)) return false;
    return true;
}
// Inverse of a symmetric positive-definite matrix; false when (near) singular.
bool invert(const std::vector<std::vector<double>>& a, std::vector<std::vector<double>>& inverse) {
    const std::size_t p = a.size();
    double scale = 0;
    for (std::size_t i = 0; i < p; ++i) scale = std::max(scale, std::fabs(a[i][i]));
    if (!(scale > 0)) return false;
    std::vector<std::vector<double>> m(p, std::vector<double>(2 * p, 0));
    for (std::size_t i = 0; i < p; ++i) {
        for (std::size_t j = 0; j < p; ++j) m[i][j] = a[i][j];
        m[i][p + i] = 1;
    }
    for (std::size_t col = 0; col < p; ++col) {
        std::size_t pivot = col;
        for (std::size_t r = col + 1; r < p; ++r)
            if (std::fabs(m[r][col]) > std::fabs(m[pivot][col])) pivot = r;
        if (std::fabs(m[pivot][col]) <= 1e-13 * scale) return false;
        std::swap(m[pivot], m[col]);
        const double d = m[col][col];
        for (auto& value : m[col]) value /= d;
        for (std::size_t r = 0; r < p; ++r)
            if (r != col) {
                const double f = m[r][col];
                for (std::size_t c = 0; c < 2 * p; ++c) m[r][c] -= f * m[col][c];
            }
    }
    inverse.assign(p, std::vector<double>(p));
    for (std::size_t i = 0; i < p; ++i)
        for (std::size_t j = 0; j < p; ++j) inverse[i][j] = m[i][p + j];
    for (std::size_t i = 0; i < p; ++i)
        if (!(inverse[i][i] > 0) || !std::isfinite(inverse[i][i])) return false;
    return true;
}

struct Run {
    std::vector<double> theta;
    double ssr{INFINITY};
    bool converged{false};
    int iterations{0};
};
} // namespace

LogisticFitResult fitLogistic(const LogisticFitRequest& request) {
    LogisticFitResult out;
    const Logistic model{request.model == LogisticModel::ThreeParameterHillOne};
    const std::size_t p = model.parameters(), n = request.concentration.size();
    auto refuse = [&](std::string reason) { out.insufficientReasons.push_back(std::move(reason)); };
    if (n != request.response.size()) throw std::invalid_argument("Fit input length mismatch");
    if (!(request.confidenceLevel > 0.5 && request.confidenceLevel < 1))
        throw std::invalid_argument("Fit confidence level must be in (0.5, 1)");
    std::vector<double> x(n);
    std::map<double, std::vector<double>> byConcentration;
    for (std::size_t i = 0; i < n; ++i) {
        const double c = request.concentration[i], y = request.response[i];
        if (!(c > 0) || !std::isfinite(c) || !std::isfinite(y)) {
            refuse("every concentration must be finite and > 0 and every response finite");
            return out;
        }
        x[i] = std::log10(c);
        byConcentration[c].push_back(y);
    }
    out.distinctConcentrations = byConcentration.size();
    if (n <= p)
        refuse("needs more unit-level points than the model's " + std::to_string(p) +
               " parameters (have " + std::to_string(n) + ")");
    if (byConcentration.size() < p)
        refuse("needs at least " + std::to_string(p) + " distinct concentrations (have " +
               std::to_string(byConcentration.size()) + ")");
    if (n > 0) {
        const auto [lo, hi] = std::minmax_element(request.response.begin(), request.response.end());
        if (*lo == *hi) refuse("response has no variation");
    }
    std::map<double, double> varianceAt;
    if (request.weighting == FitWeighting::InverseVarianceOfReplicateUnits)
        for (const auto& [c, ys] : byConcentration) {
            if (ys.size() < 2) {
                refuse("inverse-variance weighting needs >= 2 replicate units at every "
                       "concentration");
                break;
            }
            const double v = sampleVariance(ys);
            if (!(v > 0)) {
                refuse("inverse-variance weighting: zero replicate variance at a concentration");
                break;
            }
            varianceAt[c] = v;
        }
    if (!out.insufficientReasons.empty()) return out;

    out.minimumConcentration = byConcentration.begin()->first;
    out.maximumConcentration = byConcentration.rbegin()->first;
    const double xmin = std::log10(out.minimumConcentration);
    const double xmax = std::log10(out.maximumConcentration);
    std::vector<double> w(n, 1.0);
    if (!varianceAt.empty())
        for (std::size_t i = 0; i < n; ++i) w[i] = 1 / varianceAt.at(request.concentration[i]);
    const std::vector<double> lower{-INFINITY, -INFINITY, xmin - kLogEc50Margin, -kHillBound};
    const std::vector<double> upper{INFINITY, INFINITY, xmax + kLogEc50Margin, kHillBound};
    auto clamp = [&](std::vector<double>& t) {
        for (std::size_t j = 0; j < p; ++j) t[j] = std::clamp(t[j], lower[j], upper[j]);
    };
    auto ssr = [&](const std::vector<double>& t) {
        long double s = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const double r = request.response[i] - model.eval(x[i], t, nullptr);
            s += w[i] * static_cast<long double>(r) * r;
        }
        return static_cast<double>(s);
    };
    auto normal = [&](const std::vector<double>& t, std::vector<std::vector<double>>& a,
                      std::vector<double>& g) {
        a.assign(p, std::vector<double>(p, 0));
        g.assign(p, 0);
        double grad[4];
        for (std::size_t i = 0; i < n; ++i) {
            const double r = request.response[i] - model.eval(x[i], t, grad);
            for (std::size_t j = 0; j < p; ++j) {
                g[j] += w[i] * grad[j] * r;
                for (std::size_t k = 0; k < p; ++k) a[j][k] += w[i] * grad[j] * grad[k];
            }
        }
    };
    auto levenbergMarquardt = [&](std::vector<double> theta) {
        Run run;
        clamp(theta);
        double current = ssr(theta), lambda = 1e-3;
        std::vector<std::vector<double>> a;
        std::vector<double> g, step;
        for (run.iterations = 1; run.iterations <= 1000; ++run.iterations) {
            normal(theta, a, g);
            bool accepted = false;
            while (!accepted) {
                auto m = a;
                for (std::size_t j = 0; j < p; ++j) m[j][j] += lambda * std::max(a[j][j], 1e-12);
                if (!solve(m, g, step)) {
                    lambda *= 10;
                } else {
                    auto next = theta;
                    for (std::size_t j = 0; j < p; ++j) next[j] += step[j];
                    clamp(next);
                    const double candidate = ssr(next);
                    if (candidate <= current) {
                        double change = 0;
                        for (std::size_t j = 0; j < p; ++j)
                            change = std::max(change, std::fabs(next[j] - theta[j]) /
                                                          (std::fabs(theta[j]) + 1e-6));
                        const double decrease = current - candidate;
                        theta = next;
                        current = candidate;
                        lambda = std::max(lambda / 10, 1e-12);
                        accepted = true;
                        if (change < 1e-11 || decrease <= 1e-15 * current) {
                            run.converged = true;
                            run.theta = theta;
                            run.ssr = current;
                            return run;
                        }
                    } else {
                        lambda *= 10;
                    }
                }
                if (lambda > 1e16) {
                    // No descent direction: a (possibly bound-constrained) minimum.
                    run.converged = true;
                    run.theta = theta;
                    run.ssr = current;
                    return run;
                }
            }
        }
        run.iterations = 1000;
        run.theta = theta;
        run.ssr = current;
        return run;
    };
    // Starting values: asymptotes from the response at the lowest and highest
    // concentration (direction is carried by bottom/top, hill starts at +1).
    const double yLow = mean(byConcentration.begin()->second);
    const double yHigh = mean(byConcentration.rbegin()->second);
    Run best;
    for (double fraction : {0.5, 0.25, 0.75}) {
        std::vector<double> start{yLow, yHigh, xmin + fraction * (xmax - xmin)};
        if (!model.fixedHill) start.push_back(1.0);
        const auto run = levenbergMarquardt(start);
        if ((run.converged && !best.converged) ||
            (run.converged == best.converged && run.ssr < best.ssr))
            best = run;
    }
    out.fitted = true;
    out.converged = best.converged;
    out.iterations = best.iterations;
    out.degreesOfFreedom = static_cast<int>(n - p);
    out.weights = w;
    out.residualSumSquares = best.ssr;
    long double wy = 0, ws = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double f = model.eval(x[i], best.theta, nullptr);
        out.fittedValues.push_back(f);
        out.residuals.push_back(request.response[i] - f);
        wy += w[i] * request.response[i];
        ws += w[i];
    }
    const double ybar = static_cast<double>(wy / ws);
    long double total = 0;
    for (std::size_t i = 0; i < n; ++i)
        total += w[i] * static_cast<long double>(request.response[i] - ybar) *
                 (request.response[i] - ybar);
    out.rSquared = 1 - best.ssr / static_cast<double>(total);

    std::vector<std::vector<double>> a, inverse;
    std::vector<double> g;
    normal(best.theta, a, g);
    const bool covariance = invert(a, inverse);
    const double s2 = best.ssr / (n - p);
    const double q = studentTQuantile(0.5 + request.confidenceLevel / 2, double(n - p));
    const char* names[] = {"bottom", "top", "logEC50", "hill"};
    for (std::size_t j = 0; j < p; ++j) {
        FitParameter parameter;
        parameter.name = names[j];
        parameter.estimate = best.theta[j];
        parameter.atBound = (std::isfinite(lower[j]) &&
                             best.theta[j] - lower[j] <= 1e-6 * std::max(1.0, std::fabs(lower[j]))) ||
                            (std::isfinite(upper[j]) &&
                             upper[j] - best.theta[j] <= 1e-6 * std::max(1.0, std::fabs(upper[j])));
        if (covariance) {
            parameter.standardError = std::sqrt(s2 * inverse[j][j]);
            parameter.confidenceInterval = Interval{best.theta[j] - q * *parameter.standardError,
                                                    best.theta[j] + q * *parameter.standardError};
        }
        out.parameters.push_back(parameter);
    }
    auto& c = out.parameters[2];
    out.ec50 = std::pow(10.0, c.estimate);
    if (request.scale == ConcentrationScale::Log10) {
        if (c.confidenceInterval)
            out.ec50Interval = Interval{std::pow(10.0, c.confidenceInterval->lower),
                                        std::pow(10.0, c.confidenceInterval->upper)};
    } else {
        // Linear EC50 parameterization: the same least-squares minimum; the
        // linearized covariance transforms exactly by dEC50/dlogEC50 = EC50 ln 10.
        FitParameter linear;
        linear.name = "ec50";
        linear.estimate = out.ec50;
        linear.atBound = c.atBound;
        if (c.standardError) {
            linear.standardError = out.ec50 * kLn10 * *c.standardError;
            linear.confidenceInterval = Interval{out.ec50 - q * *linear.standardError,
                                                 out.ec50 + q * *linear.standardError};
            out.ec50Interval = linear.confidenceInterval;
        }
        c = linear;
    }

    if (!out.converged)
        out.suspectReasons.push_back("not converged after " + std::to_string(out.iterations) +
                                     " Levenberg-Marquardt iterations");
    if (out.ec50 < out.minimumConcentration || out.ec50 > out.maximumConcentration)
        out.suspectReasons.push_back("EC50 outside the tested concentration range");
    if (!covariance) {
        out.suspectReasons.push_back(
            "parameter covariance is singular; standard errors and CIs unavailable");
    } else if (out.ec50Interval) {
        const bool wide =
            request.scale == ConcentrationScale::Log10
                ? !(out.ec50Interval->lower > 0) ||
                      std::log10(out.ec50Interval->upper / out.ec50Interval->lower) >
                          xmax - xmin
                : out.ec50Interval->upper - out.ec50Interval->lower >
                      out.maximumConcentration - out.minimumConcentration;
        if (wide || !std::isfinite(out.ec50Interval->upper))
            out.suspectReasons.push_back(
                "EC50 confidence interval wider than the tested concentration range");
    }
    if (out.distinctConcentrations < kSuspectBelowConcentrations)
        out.suspectReasons.push_back("fewer than " + std::to_string(kSuspectBelowConcentrations) +
                                     " distinct concentrations");
    for (const auto& parameter : out.parameters)
        if (parameter.atBound)
            out.suspectReasons.push_back("parameter at bound: " + parameter.name);
    return out;
}

} // namespace backend::analysis::inference
