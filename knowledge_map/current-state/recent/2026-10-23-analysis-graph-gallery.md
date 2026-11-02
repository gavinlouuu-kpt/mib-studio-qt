## 2026-10-23 — Production analysis graph and gated gallery

The shared Studio/Review PopulationAnalysis now submits native interval/Boolean
graphs and displays exact selected-observation thumbnail pages. Evaluation and
image copies share the ReviewSession source lock and existing admission fences.
Graph recipes can be saved/restored in the current browser; results and source
identity are not persisted. See [[services/AnalysisWorker]] and
`docs/design/flowjo-analysis/app-qa.md` for scope and validation.
