## 2026-10-18 — One review implementation: execution plan (#512)

Track B (#717) step B0. [`docs/exec-plans/active/2026-10-18-one-review-implementation.md`](../../../docs/exec-plans/active/2026-10-18-one-review-implementation.md)
surveys MIB Studio's review stack (develop #450 on `BackendFacade`) against
YOFO Review's ([[ReviewSession]], [[YofoReview]]) and orders the work into
one-PR slices: engines into `mib_review_core` first (overlay kernel,
reanalysis, export, charts JSON), then the facade owning the session, the
parity harness's MIB Studio half, job lanes, YOFO regenerate on the shared
engine and a transport-neutral review module — all with no contract change —
then the three contract changes (attachable review bridge, review commands in
the studio command layer and the WebSocket server, MIB Studio's options in
the review contract; ABI numbers left to the coordinator) and B5, MIB
Studio's Review stage mounting `desktop/src/review/`. Nine decisions (D1-D9)
are listed for the owner, among them experiments while a review file is open,
one ROI style and the factor policy. Linked from the standalone review-app
plan's decision log.
