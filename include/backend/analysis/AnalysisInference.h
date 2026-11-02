#pragma once
// F-4 (#808 F09, I10): the native inference authority (ADR 0016). Pure C++,
// no JSON, no I/O: distribution functions, two-group / paired / k-group tests
// on experimental-unit values, and the concentration-response fit. Inputs are
// unit-level values only (one value per independent experimental unit); event
// rows are never units. Callers (AnalysisComparison.cpp) decide sufficiency
// and serialization. Reference values: tools/analysis_inference/
// generate_reference.py (scipy), checked in tests/analysis/inference_test.cpp.
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace backend::analysis::inference {

// ---- distributions (standard algorithms) ---------------------------------
// Regularized incomplete beta I_x(a, b): Lentz continued fraction
// (Numerical Recipes betacf), symmetry x > (a+1)/(a+b+2).
double regularizedIncompleteBeta(double a, double b, double x);
// Regularized lower incomplete gamma P(a, x): series for x < a+1, continued
// fraction otherwise.
double regularizedLowerGamma(double a, double x);
double normalCdf(double z);       // 0.5 * erfc(-z / sqrt 2)
double normalQuantile(double p);  // Acklam rational approximation + one Halley step
double studentTCdf(double t, double df);
double studentTQuantile(double p, double df); // bracketed bisection on studentTCdf
double fSurvival(double f, double d1, double d2);
double chiSquareSurvival(double x, double k);

// ---- tests ---------------------------------------------------------------
struct Interval {
    double lower{0}, upper{0};
};
struct TestResult {
    std::string test;          // "welch-t", "mann-whitney-u", ...
    std::string statisticName; // "t", "U", "W", "F", "H"
    double statistic{0};
    std::optional<double> df1, df2;
    double pValue{1};
    std::string pMethod;       // how the p-value was obtained (exact / distribution)
    std::string estimateName;  // e.g. "difference of means (comparator - reference)"
    std::optional<double> estimate;
    std::optional<Interval> confidenceInterval;
    std::string ciUnavailableReason;
    std::string effectSizeName;
    std::optional<double> effectSize;
    std::vector<std::string> notes;
};

// Two independent groups; differences are comparator - reference.
TestResult welchT(const std::vector<double>& reference, const std::vector<double>& comparator,
                  double confidenceLevel);
// Exact null distribution (no ties, both n <= 50) else normal approximation
// with tie correction and continuity correction 0.5. U is the comparator's.
TestResult mannWhitneyU(const std::vector<double>& reference,
                        const std::vector<double>& comparator);
// Paired: d = comparator - reference per unit.
TestResult pairedT(const std::vector<double>& reference, const std::vector<double>& comparator,
                   double confidenceLevel);
// Zero differences dropped (Wilcoxon's method). Exact when n <= 50 and no
// ties, else normal approximation with tie correction, no continuity
// correction. Statistic is min(W+, W-).
TestResult wilcoxonSignedRank(const std::vector<double>& reference,
                              const std::vector<double>& comparator);
TestResult oneWayAnova(const std::vector<std::vector<double>>& groups);
TestResult kruskalWallis(const std::vector<std::vector<double>>& groups);

// ---- concentration-response fit (I10) -------------------------------------
enum class LogisticModel { FourParameter, ThreeParameterHillOne };
enum class ConcentrationScale { Log10, Linear };
enum class FitWeighting { None, InverseVarianceOfReplicateUnits };
struct LogisticFitRequest {
    std::vector<double> concentration; // > 0, in the declared unit
    std::vector<double> response;      // one unit-level value per point
    LogisticModel model{LogisticModel::FourParameter};
    ConcentrationScale scale{ConcentrationScale::Log10};
    FitWeighting weighting{FitWeighting::None};
    double confidenceLevel{0.95};
};
struct FitParameter {
    std::string name;
    double estimate{0};
    std::optional<double> standardError;
    std::optional<Interval> confidenceInterval;
    bool atBound{false};
};
struct LogisticFitResult {
    bool fitted{false};                        // false: insufficient, nothing below is set
    std::vector<std::string> insufficientReasons;
    bool converged{false};
    int iterations{0};
    std::vector<FitParameter> parameters;      // bottom, top, logEC50|ec50, hill (4PL)
    double ec50{0};
    std::optional<Interval> ec50Interval;
    std::vector<double> weights, fittedValues, residuals;
    double residualSumSquares{0}, rSquared{0};
    int degreesOfFreedom{0};
    std::size_t distinctConcentrations{0};
    double minimumConcentration{0}, maximumConcentration{0};
    std::vector<std::string> suspectReasons;
};
// Levenberg-Marquardt on y = bottom + (top - bottom) / (1 + 10^((logEC50 - log10 c) * hill)),
// 3PL fixes hill = 1. Covariance s^2 (J'WJ)^-1 with s^2 = WSSR / (n - p).
LogisticFitResult fitLogistic(const LogisticFitRequest& request);

// Bounds the fit enforces (and reports as "parameter at bound").
inline constexpr double kHillBound = 10.0;
inline constexpr double kLogEc50Margin = 3.0; // decades beyond the tested range
inline constexpr std::size_t kSuspectBelowConcentrations = 5;

} // namespace backend::analysis::inference
