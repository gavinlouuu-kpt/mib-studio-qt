## 2026-10-08 — One validated config.json applier for profiles and central methods (#398 M2c)

Local profiles (`app/ProfileStore.cpp`) and central methods (`app::applyCentralMethod`)
had two appliers with different rules. Both now use one core in `ConfigDocumentApply.h`:
`configApplyBlocker` (raw recording, capture, realtime, autofocus), `stageConfigDocument`
(validates everything, changes nothing) and `commitStagedConfig`. The local-profile
rules win: bounded numbers, ROI validated against a captured preview,
`realtime_processing.enabled` / `drop_frames`, the stage block, strict delivery and
realtime modes. Kept from the Qt AppConfigWatcher: the v2 `difference_threshold` key,
the root contract version, and a multi-image count clamped to 1. These now also apply
to local profiles. Profiles still record a provenance document; central methods record
their exact text (the `method.revision` gate matches it). `applyCentralMethod` now runs
inside the coordinator's idle transaction (as profiles do), so Start cannot interleave.
A run left Failed and not yet cleared blocks it too. Stricter for central methods: an ROI
needs a captured preview, and out-of-range values fail instead of being clamped.
See [[services/ProfileRegistryService]] and [[architecture/Desktop-Shell]].
Guards: `backend.config_document_apply` (ROI against a preview, the shared bounds),
`backend.profile_store` (difference_threshold), `e2e.method_gate`.
