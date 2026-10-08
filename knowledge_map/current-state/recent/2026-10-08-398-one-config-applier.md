## 2026-10-08 — One validated config.json applier for profiles and central methods (#398 M2c)

Local profiles (`app/ProfileStore.cpp`) and central methods (`app::applyCentralMethod`)
had two appliers with different rules. Both now use one core in `ConfigDocumentApply.h`:
`configApplyBlocker` (raw recording, capture, realtime, autofocus), `stageConfigDocument`
(validates everything, changes nothing) and `commitStagedConfig`. The local-profile
rules win: bounded numbers (errors name the field and its bounds),
`realtime_processing.enabled` / `drop_frames`, the stage block, strict delivery and
realtime modes. Kept from the Qt AppConfigWatcher: the v2 `difference_threshold` key,
the root contract version, and a multi-image count clamped to 1. These now also apply
to local profiles. Profiles still record a provenance document; central methods record
their exact text (the `method.revision` gate matches it). `applyCentralMethod` now runs
inside the coordinator's idle transaction (as profiles do), so Start cannot interleave.
A run left Failed and not yet cleared blocks it too. Out-of-range values now fail
for central methods instead of being clamped.

**ROI rule (`checkRoi`):** validated against the latest captured frame; with no frame yet,
against the selected camera's capture window (or sensor) when known; with neither, the rest
of the document applies and the ROI is pending (`ProcessingService::setPendingRealtimeRoi`).
The realtime loop applies it on the first frame it sees if it fits, otherwise keeps the
previous ROI and records a notice (readiness `processing.roi` warns). While pending,
`processing.roi` fails, so Start waits. This keeps the start-of-day workflow (apply a method
before Live View) working, for profiles and central methods alike. The Methods panel shows
the backend's reason verbatim and words a pending ROI as "applied on the first captured frame".
See [[services/ProfileRegistryService]] and [[architecture/Desktop-Shell]].
Guards: `backend.config_document_apply` (the ROI rule's three cases, pending then applied on
the first frame, a non-fitting pending ROI dropped with a notice, the readiness gate, the
shared bounds and their messages), `backend.profile_store` (difference_threshold, bounds,
pending ROI on the profile path), `e2e.method_gate`, `registry.test.ts`.
