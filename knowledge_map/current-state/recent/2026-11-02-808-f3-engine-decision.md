## 2026-11-02 — F-3: native analysis authority applied to the F docs (#808)

F-3 of the FlowJo fold-in (decision D3, ADR 0016; goals F08/F04 comparators).
The F ledger, design docs and [[services/AnalysisWorker]] now name the native
C++ code as the single production authority. Cytoflow 1.3.2 and FlowKit 1.3.2
are CI comparators, not bundled; t-SNE stays in the worker with scikit-learn
1.9.1 (`sklearn-tsne-1.9.1`). `tools/flowjo_acceptance/fcs_comparator.py
--flowkit` gates a rectangle with FlowKit's own GatingML gate, and
`analysis.fcs_reader` compares it with the native count when
`MIB_FCS_COMPARATOR_PYTHON` and `MIB_FCS_COMPARATOR_FLOWKIT` are set. The
comparator also reads channel names from flowio >= 1.3.
