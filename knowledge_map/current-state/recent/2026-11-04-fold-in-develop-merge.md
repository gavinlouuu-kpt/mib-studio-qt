## 2026-11-04 — Fold-in E/I/F lands on develop through #816; three lanes the stacks never ran

The reviewed fold-in integration (`claude/fold-in-integration-e2e`: E #812 → #817 → #819,
F #810 → #818 → #823 → #824, I #820 → #821 → #822 → #825) was merged into #816
(`claude/serene-wozniak-xkgq2i`, base `develop`) so one composition lands. Several lanes only
run on PRs into `develop`, so the stacked PRs had never built them. Each one failed once on
the combined tree:

- **`analysis-ci.yml` `native-and-worker`:** `tools/flowjo_acceptance/CMakeLists.txt` keeps
  its own `mib_analysis_host` source list, and that list was missing `RecipeProvider.cpp` and
  `NextRunPlanning.cpp`, so `analysis_host_test` hit a link error. Both are added; see
  [[build-and-run/Build]].
- **`review-bundles.yml` (Windows):** `mib_review_core` now links `SqliteService` (the
  analysis host's workspace and recipe store), but the Conan `review_core=True` graph had no
  sqlite3 (C1083 `sqlite3.h`). sqlite3 is now required in both graphs, static in the review
  graph; see [[build-and-run/Dependencies]].
- **`qt-ci.yml` Windows MSVC:** `review_population_scale_test` and
  `review_workspace_scale_test` called glibc's `malloc_trim` without a guard. A `trimHeap()`
  helper now calls it only under `__GLIBC__`. Their memory bounds read `/proc/self/status`,
  so they are measured on Linux only.
  `fcs_reader_test` and `flowjo_reference_test` call `popen` / `pclose` for the optional
  comparator; on `_WIN32` these now map to `_popen` / `_pclose`.

The bridge ABI numbers 48–50 still need confirming in #716. The macOS
`run_statistics_twin_over_the_parity_fixture` last-bit failure fails on `develop` too, and
fixes are open in #830 and #832.
