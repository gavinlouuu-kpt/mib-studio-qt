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
(`tools/analysis_inference/generate_reference.py`). See [[services/AnalysisWorker]].
