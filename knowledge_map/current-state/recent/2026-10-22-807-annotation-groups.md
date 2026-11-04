## 2026-10-22 — #807 iQue audit and bounded annotation grouping

The iQue coordinator recovered original concept revision 71 and pinned the
fold-in/O/R native inventory at revision 284. The full I01–I12 ledger, restored
scope, v1 contract proposals, dependency owners and I13 deferral are in
`docs/design/ique-workflow/README.md`. No full capability acceptance is claimed.

`workspace/annotationModel.ts` projects existing `PlanDraft` annotations into
fixed/custom-key groups, preserving missing units and declared unit labels.
`AnnotationGroups.tsx` is mounted in both Plan views. It reuses the kit and the
production Start serializer to expose validated version-1 annotation JSON;
no scientific calculation, persistence authority, run identity or command is
added. Grouping does not reorder the queue. Group selection is not persistent;
copying JSON is not save/reopen. See [[architecture/Desktop-Shell]] and the
reproducible demo in `docs/design/ique-workflow/DEMO.md`.

Shared analysis (#808/#523), durable workspace/recipe/finding storage
(#393/#395/#808), complete acquisition qualification (#806) and trend contracts
remain blockers. No promotion, deployment, cross-track merge or hardware action.
