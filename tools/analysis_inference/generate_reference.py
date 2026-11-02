#!/usr/bin/env python3
"""Independent reference values for the native inference authority (#808 F-4).

F09 / I10: the native C++ tests (tests/analysis/inference_test.cpp,
tests/analysis/comparison_spec_test.cpp) compare against values computed here
with scipy, not with the code under test. This script lives outside the
production path (ADR 0016: scipy is a comparator only).

    python3 -m pip install scipy numpy
    python3 tools/analysis_inference/generate_reference.py          # rewrite header
    python3 tools/analysis_inference/generate_reference.py --check  # exit 1 on drift

Writes tests/analysis/inference_reference.h. The datasets are fixed literals
(no RNG at generation time); the 4PL data were drawn once and rounded.
"""
from __future__ import annotations

import argparse
import pathlib
import sys

import numpy as np
import scipy
from scipy import optimize, stats

ROOT = pathlib.Path(__file__).resolve().parents[2]
HEADER = ROOT / "tests" / "analysis" / "inference_reference.h"

# ---- datasets ---------------------------------------------------------------
WELCH_REF = [19.8, 20.4, 19.6, 17.8, 18.5, 18.9, 18.3, 18.9, 19.5, 22.0]
WELCH_CMP = [28.2, 26.6, 20.1, 23.3, 25.2, 22.1, 17.7, 27.6, 20.6, 13.7,
             23.2, 17.5, 20.6, 18.0, 23.9, 21.6, 24.3, 20.4, 23.9, 13.3]
MWU_TIES_REF = [1.1, 2.2, 2.2, 3.5, 4.0, 4.0, 5.1]
MWU_TIES_CMP = [2.2, 3.0, 4.0, 4.8, 6.0, 6.0, 7.2, 8.1]
MWU_EXACT_REF = [3.1, 4.5, 2.2, 5.0, 3.9]
MWU_EXACT_CMP = [6.1, 5.5, 7.0, 4.8, 6.6, 5.9]
PAIRED_REF = [200, 174, 198, 170, 179, 182, 193, 209, 185, 155, 169, 210]
PAIRED_CMP = [191, 170, 177, 167, 159, 151, 176, 183, 159, 145, 146, 177]
# Darwin's Zea mays cross- minus self-fertilized differences (Fisher 1935).
DARWIN_D = [6, 8, 14, 16, 23, 24, 28, 29, 41, -48, 49, 56, 60, -67, 75]
WILCOXON_EXACT_REF = [100.0 + i for i in range(len(DARWIN_D))]
WILCOXON_EXACT_CMP = [r + d for r, d in zip(WILCOXON_EXACT_REF, DARWIN_D)]
WILCOXON_TIES_REF = [10, 12, 15, 11, 9, 14, 13, 10, 12, 16, 11]
WILCOXON_TIES_CMP = [12, 15, 15, 13, 13, 11, 16, 14, 12, 19, 14]
# McDonald et al. (1991) mussel shell AAM length ratios (scipy f_oneway docs).
ANOVA_GROUPS = [
    [0.0571, 0.0813, 0.0831, 0.0976, 0.0817, 0.0859, 0.0735, 0.0659, 0.0923, 0.0836],
    [0.0873, 0.0662, 0.0672, 0.0819, 0.0749, 0.0649, 0.0835, 0.0725],
    [0.0974, 0.1352, 0.0817, 0.1016, 0.0968, 0.1064, 0.105],
    [0.1033, 0.0915, 0.0781, 0.0685, 0.0677, 0.0697, 0.0764, 0.0689],
    [0.0703, 0.1026, 0.0956, 0.0973, 0.1039, 0.1045],
]
# 4PL: 7 concentrations (nM) x 3 units, drawn once from bottom 5, top 95,
# EC50 3 nM, hill 1.2 with heteroscedastic noise, rounded to 0.01.
FIT_CONC = [c for c in [0.1, 0.3, 1, 3, 10, 30, 100] for _ in range(3)]
FIT_RESP = [4.19, 8.26, 7.9, 8.04, 10.41, 10.74, 15.88, 24.87, 24.97, 50.49, 49.47,
            47.77, 74.97, 94.11, 80.98, 86.51, 91.76, 100.39, 87.91, 104.93, 89.88]
# Suspect: the tested range (0.03..10 nM) stops below the generating EC50
# (30 nM; bottom 10, top 100, hill 1, noise SD 0.4), so the fitted EC50 lies
# outside the tested range.
SUSPECT_CONC = [c for c in [0.03, 0.1, 0.3, 1, 3, 10] for _ in range(3)]
SUSPECT_RESP = [10.19, 10.38, 9.92, 10.83, 10.73, 10.35, 11.05, 11.13, 10.59, 12.5, 13.5,
                13.77, 18.27, 18.29, 17.82, 32.65, 31.81, 32.65]
CONFIDENCE = 0.95


def logistic(x, b, t, c, h):
    return b + (t - b) / (1 + 10 ** ((c - x) * h))


def logistic3(x, b, t, c):
    return logistic(x, b, t, c, 1.0)


def logistic_linear(conc, b, t, ec50, h):
    return b + (t - b) / (1 + (ec50 / conc) ** h)


def fit(model, xdata, y, p0, sigma=None):
    params, cov = optimize.curve_fit(model, xdata, y, p0=p0, sigma=sigma, absolute_sigma=False,
                                     method="lm", ftol=1e-15, xtol=1e-15, gtol=1e-15,
                                     maxfev=200000)
    n, p = len(y), len(params)
    se = np.sqrt(np.diag(cov))
    q = stats.t.ppf(0.5 + CONFIDENCE / 2, n - p)
    w = np.ones(n) if sigma is None else 1 / np.asarray(sigma) ** 2
    resid = np.asarray(y) - model(np.asarray(xdata), *params)
    ybar = np.sum(w * y) / np.sum(w)
    r2 = 1 - np.sum(w * resid ** 2) / np.sum(w * (np.asarray(y) - ybar) ** 2)
    return params, se, q, r2


def values() -> dict[str, object]:
    out: dict[str, object] = {}
    # distributions
    out["tCdf_m1p5_df7"] = stats.t.cdf(-1.5, 7)
    out["tCdf_2p2_df3p5"] = stats.t.cdf(2.2, 3.5)
    out["tPpf_0p975_df4"] = stats.t.ppf(0.975, 4)
    out["tPpf_0p995_df27p3"] = stats.t.ppf(0.995, 27.3)
    out["fSf_3p1_df4_df20"] = stats.f.sf(3.1, 4, 20)
    out["chi2Sf_7p8_df3"] = stats.chi2.sf(7.8, 3)
    out["chi2Sf_40_df10"] = stats.chi2.sf(40, 10)
    out["normPpf_0p001"] = stats.norm.ppf(0.001)
    out["normPpf_0p8"] = stats.norm.ppf(0.8)
    out["betainc_2p5_4_0p3"] = scipy.special.betainc(2.5, 4, 0.3)

    welch = stats.ttest_ind(WELCH_CMP, WELCH_REF, equal_var=False)
    ci = welch.confidence_interval(CONFIDENCE)
    out.update(welchT=welch.statistic, welchDf=welch.df, welchP=welch.pvalue,
               welchCiLower=ci.low, welchCiUpper=ci.high)

    mwu = stats.mannwhitneyu(MWU_TIES_CMP, MWU_TIES_REF, alternative="two-sided",
                             method="asymptotic", use_continuity=True)
    out.update(mwuTiesU=mwu.statistic, mwuTiesP=mwu.pvalue)
    mwu = stats.mannwhitneyu(MWU_EXACT_CMP, MWU_EXACT_REF, alternative="two-sided",
                             method="exact")
    out.update(mwuExactU=mwu.statistic, mwuExactP=mwu.pvalue)

    paired = stats.ttest_rel(PAIRED_CMP, PAIRED_REF)
    ci = paired.confidence_interval(CONFIDENCE)
    out.update(pairedT=paired.statistic, pairedDf=paired.df, pairedP=paired.pvalue,
               pairedCiLower=ci.low, pairedCiUpper=ci.high)

    wil = stats.wilcoxon(np.subtract(PAIRED_CMP, PAIRED_REF), zero_method="wilcox",
                         correction=False, alternative="two-sided", method="approx")
    out.update(pairedWilcoxonW=wil.statistic, pairedWilcoxonP=wil.pvalue)
    mwu = stats.mannwhitneyu(WELCH_CMP, WELCH_REF, alternative="two-sided",
                             method="asymptotic", use_continuity=True)
    out.update(welchDataMwuU=mwu.statistic, welchDataMwuP=mwu.pvalue)
    out.update(welchRefMean=np.mean(WELCH_REF), welchRefMedian=np.median(WELCH_REF))

    d = np.subtract(WILCOXON_EXACT_CMP, WILCOXON_EXACT_REF)
    wil = stats.wilcoxon(d, zero_method="wilcox", alternative="two-sided", method="exact")
    out.update(wilcoxonExactW=wil.statistic, wilcoxonExactP=wil.pvalue)
    d = np.subtract(WILCOXON_TIES_CMP, WILCOXON_TIES_REF)
    wil = stats.wilcoxon(d, zero_method="wilcox", correction=False, alternative="two-sided",
                         method="approx")
    out.update(wilcoxonTiesW=wil.statistic, wilcoxonTiesP=wil.pvalue)

    anova = stats.f_oneway(*ANOVA_GROUPS)
    out.update(anovaF=anova.statistic, anovaP=anova.pvalue)
    kw = stats.kruskal(*ANOVA_GROUPS)
    out.update(kruskalH=kw.statistic, kruskalP=kw.pvalue)

    x = np.log10(FIT_CONC)
    y = np.asarray(FIT_RESP)
    params, se, q, r2 = fit(logistic, x, y, [7.0, 94.0, 0.5, 1.0])
    out.update(fitBottom=params[0], fitTop=params[1], fitLogEc50=params[2], fitHill=params[3],
               fitSeBottom=se[0], fitSeTop=se[1], fitSeLogEc50=se[2], fitSeHill=se[3],
               fitEc50=10 ** params[2], fitEc50Lower=10 ** (params[2] - q * se[2]),
               fitEc50Upper=10 ** (params[2] + q * se[2]), fitR2=r2)
    sd = np.array([np.std(y[i // 3 * 3:i // 3 * 3 + 3], ddof=1) for i in range(len(y))])
    params, se, q, r2 = fit(logistic, x, y, [7.0, 94.0, 0.5, 1.0], sigma=sd)
    out.update(wfitBottom=params[0], wfitTop=params[1], wfitLogEc50=params[2],
               wfitHill=params[3], wfitSeLogEc50=se[2], wfitSeHill=se[3],
               wfitEc50=10 ** params[2], wfitR2=r2)
    params, se, q, r2 = fit(logistic3, x, y, [7.0, 94.0, 0.5])
    out.update(fit3Bottom=params[0], fit3Top=params[1], fit3LogEc50=params[2],
               fit3SeLogEc50=se[2], fit3R2=r2)
    params, se, q, _ = fit(logistic_linear, np.asarray(FIT_CONC, dtype=float), y,
                           [7.0, 94.0, 3.0, 1.0])
    out.update(linEc50=params[2], linSeEc50=se[2], linEc50Lower=params[2] - q * se[2],
               linEc50Upper=params[2] + q * se[2])
    params, se, q, _ = fit(logistic, np.log10(SUSPECT_CONC), np.asarray(SUSPECT_RESP),
                           [10.0, 32.0, -0.26, 1.0])
    out.update(suspectEc50=10 ** params[2], suspectHill=params[3])
    return out


def literal(values: list[float]) -> str:
    return "{" + ", ".join(repr(float(v)) for v in values) + "}"


def render() -> str:
    data = {
        "welchReference": WELCH_REF, "welchComparator": WELCH_CMP,
        "mwuTiesReference": MWU_TIES_REF, "mwuTiesComparator": MWU_TIES_CMP,
        "mwuExactReference": MWU_EXACT_REF, "mwuExactComparator": MWU_EXACT_CMP,
        "pairedReference": PAIRED_REF, "pairedComparator": PAIRED_CMP,
        "wilcoxonExactReference": WILCOXON_EXACT_REF,
        "wilcoxonExactComparator": WILCOXON_EXACT_CMP,
        "wilcoxonTiesReference": WILCOXON_TIES_REF, "wilcoxonTiesComparator": WILCOXON_TIES_CMP,
        "fitConcentration": FIT_CONC, "fitResponse": FIT_RESP,
        "suspectConcentration": SUSPECT_CONC, "suspectResponse": SUSPECT_RESP,
    }
    lines = [
        "// GENERATED by tools/analysis_inference/generate_reference.py; do not edit.",
        f"// scipy {scipy.__version__}, numpy {np.__version__}. Independent reference values",
        "// for the native inference authority (#808 F-4: F09, I10).",
        "#pragma once",
        "#include <vector>",
        "namespace inference_reference {",
    ]
    for name, values_ in data.items():
        lines.append(f"inline const std::vector<double> {name}{literal(values_)};")
    lines.append("inline const std::vector<std::vector<double>> anovaGroups{" +
                 ", ".join(literal(g) for g in ANOVA_GROUPS) + "};")
    for name, value in values().items():
        lines.append(f"constexpr double {name} = {float(value)!r};")
    lines.append("} // namespace inference_reference")
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="fail if the header is stale")
    args = parser.parse_args()
    text = render()
    if args.check:
        current = HEADER.read_text(encoding="utf-8") if HEADER.exists() else ""
        # The version line may differ between hosts; compare the values only.
        strip = lambda t: "\n".join(l for l in t.splitlines() if not l.startswith("// scipy"))
        if strip(current) != strip(text):
            print(f"{HEADER} is stale; rerun without --check", file=sys.stderr)
            return 1
        return 0
    HEADER.write_text(text, encoding="utf-8")
    print(f"wrote {HEADER}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
