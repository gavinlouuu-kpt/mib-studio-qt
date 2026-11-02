## 2026-11-02 — F-4: declared-unit comparison and concentration-response fits (#808)

F-4 of the FlowJo fold-in (goals F09, I09/I10 backend). The native C++
authority (ADR 0016) now owns inference and fits:

- **ComparisonSpec** (`compareSpec`, host action `comparisonSpec`): metric =
  population × parameter × statistic of each pinned revision (exact statistics),
  declared experimental units, groups, design (unpaired | paired by unit id),
  inference (none | parametric | rank | both). Incompatible sources are
  excluded with reasons; inference needs >= 3 units per group, else
  `insufficient` and no p-value.
- **Fit authority** (`fitSpec`, host action `concentrationFit`): 4PL/3PL by
  Levenberg-Marquardt, declared concentration unit and log10/linear scale,
  weighting none or 1/variance of replicate units, SE/CI, EC50 CI, residuals,
  R^2 and suspect-fit reasons; insufficient input yields no fit.
- Review ABI stays 10 (additive to the unmerged contract); bridge ABI 47 unchanged.

Proof: `analysis.inference`, `analysis.comparison_spec` against scipy values
(`tools/analysis_inference/generate_reference.py`).

Review fixes (#824; F09, I10):
- **Undefined tests.** Degenerate inputs no longer report p = 1 or p = 0 as
  `computed`. `TestResult` has `status` (`computed` | `undefined`),
  `undefinedReason`, and nullable `statistic` / `pValue`: zero within-group variance
  (Welch, ANOVA), zero variance of paired differences (paired t, e.g. every
  difference +1), every paired difference zero (Wilcoxon), all values tied
  (Kruskal-Wallis, Mann-Whitney), and any non-finite intermediate (values near
  1e200 / 1e308) give `undefined` with a reason and no statistic, p or CI. The
  comparison is `undefined` only when no requested test is defined; each test
  carries its own status.
- **Fit suspect flags.** The "EC50 CI wider than the tested range" rule uses the
  log10 interval in both reporting scales; a linear-scale Wald interval reaching
  <= 0 is flagged; Levenberg-Marquardt hitting damping > 1e16 is `termination:
  stalled` (not converged, a suspect reason); inverse-variance weights from fewer
  than 3 replicates per concentration are flagged.
- **Confidence level.** One rule, `inference::checkConfidenceLevel`: (0.5, 0.999]
  for comparisons, tests and fits.
- **Contract.** Review ABI 10 text (unmerged) and the bridge contract describe the
  nullable `pValue`, `undefined` status and `termination`.
Proof: `analysis.inference` (n = 3 cases, Hedges g, rank-biserial, dz, eta^2,
epsilon^2 and Hodges-Lehmann against scipy/numpy; degenerate and overflow cases;
bounded hill `atBound`; the linear-scale review dataset), `analysis.comparison_spec`
(undefined through the host, confidence 0.5 refused, `termination`). See [[services/AnalysisWorker]].
