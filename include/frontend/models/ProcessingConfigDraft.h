// Processing tune-panel draft model + acknowledged apply contract (issue #364).
//
// The Monitoring "tune" panel exposes a subset of ProcessingConfig (the cell
// acceptance criteria, the target-group sorting gate and multi-image
// acquisition). This header owns the field mapping and the draft semantics so
// they are testable without widgets:
//
//  - TuneField enumerates every exposed field with its full name, unit,
//    display precision and JSON path (image_processing/...).
//  - ProcessingConfigPatch carries only the fields the operator deliberately
//    changed. Untouched fields are never rewritten, so a high-precision value
//    that a spin box merely displays with fewer decimals survives an Apply.
//  - ProcessingConfigDraft holds the authoritative baseline, the draft values,
//    the changed-field set, and the dirty / conflict / applying state. It is
//    independent of widget visibility.
//  - ApplyProcessingDraftRequest / ConfigApplyResult are the request/result
//    contract between the panel and the config coordination layer
//    (AppConfigWatcher): Dirty clears only when persistence *and* runtime
//    application are confirmed.
#pragma once

#include "backend/processing/ProcessingTypes.h"

#include <QByteArray>
#include <QJsonObject>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <cstdint>
#include <map>
#include <vector>

namespace frontend {

enum class TuneField {
    AreaEnabled,
    AreaMin,
    AreaMax,
    DeformabilityEnabled,
    DeformabilityMin,
    DeformabilityMax,
    RingRatioEnabled,
    RingRatioMin,
    RingRatioMax,
    AreaRatioEnabled,
    AreaRatioMax,
    BorderEnabled,
    SingleInnerContourEnabled,
    TargetGroupEnabled,
    TargetAreaMin,
    TargetAreaMax,
    TargetDeformabilityMin,
    TargetDeformabilityMax,
    MultiImageEnabled,
    MultiImageCount,
};

// Which logical group a field belongs to (acceptance criteria vs sorting gate
// vs acquisition); used for labels/accessible names so the two gates cannot
// be confused.
enum class TuneGroup { Acceptance, TargetGroup, MultiImage };

const std::vector<TuneField>& allTuneFields();
const char* toString(TuneField field);
TuneGroup tuneFieldGroup(TuneField field);
// Full scientific name with unit where meaningful, e.g. "Area minimum (µm²)".
QString tuneFieldLabel(TuneField field);
// Unit suffix for value controls ("" when unitless).
QString tuneFieldUnit(TuneField field);
// Decimals the panel displays for a floating-point field (0 for int/bool).
int tuneFieldDecimals(TuneField field);
bool tuneFieldIsBool(TuneField field);
// JSON location under the config root, '/'-separated, e.g.
// "image_processing/filters/enable_area_range_check".
QString tuneFieldJsonPath(TuneField field);

QVariant readTuneField(const backend::services::ProcessingConfig& cfg, TuneField field);
void writeTuneField(backend::services::ProcessingConfig& cfg, TuneField field, const QVariant& value);
// Equality at the field's display precision: the operator cannot distinguish
// 0.123456 from 0.12 in a 2-decimal control, so re-typing the displayed
// value is not an edit.
bool tuneValuesEqual(TuneField field, const QVariant& a, const QVariant& b);

struct ProcessingConfigPatch {
    std::map<TuneField, QVariant> values;
    bool empty() const { return values.empty(); }
};

// Merge only the patched fields into `target`.
backend::services::ProcessingConfig applyTunePatch(backend::services::ProcessingConfig target, const ProcessingConfigPatch& patch);
// Write only the patched fields into the JSON document (creating the
// image_processing/filters/target_group/multi_image objects as needed);
// every other key is preserved byte-for-byte in value terms.
void applyTunePatchToJson(QJsonObject& root, const ProcessingConfigPatch& patch);
// True when every patched field reads back equal (at display precision).
bool tunePatchSatisfiedBy(const backend::services::ProcessingConfig& cfg, const ProcessingConfigPatch& patch);
// Range validation for the exposed criteria (min <= max for enabled ranges,
// multi-image count >= 1). Field-local messages; never changes values.
QStringList validateTuneConfig(const backend::services::ProcessingConfig& cfg);

struct ApplyProcessingDraftRequest {
    uint64_t requestId{0};
    QString path;                 // empty = the coordinator's active document
    QByteArray baselineFingerprint; // document fingerprint the draft was loaded against (empty = unknown)
    ProcessingConfigPatch patch;
};

struct ConfigApplyResult {
    uint64_t requestId{0};
    bool persisted{false};   // the document on disk carries the patch
    bool applied{false};     // the processing service reports the patched values
    bool conflict{false};    // the document changed since the draft baseline; nothing written
    QString error;           // human-readable failure / partial-outcome text
    QByteArray fingerprint;  // document fingerprint after a successful write
    backend::services::ProcessingConfig effectiveConfig; // runtime config after the attempt
    bool ok() const { return persisted && applied && !conflict; }
};

class ProcessingConfigDraft {
public:
    enum class ExternalOutcome { Refreshed, Unchanged, Conflict, Deferred };

    // Replace baseline + draft; clears changes, conflict and errors.
    void loadBaseline(const backend::services::ProcessingConfig& cfg);
    const backend::services::ProcessingConfig& baseline() const { return baseline_; }
    const backend::services::ProcessingConfig& draft() const { return draft_; }

    // Operator edit. Returns true when the dirty/changed set changed.
    bool setField(TuneField field, const QVariant& value);
    QVariant field(TuneField field) const { return readTuneField(draft_, field); }
    QVariant baselineField(TuneField field) const { return readTuneField(baseline_, field); }

    bool dirty() const { return !changed_.empty(); }
    int changeCount() const { return static_cast<int>(changed_.size()); }
    std::vector<TuneField> changedFields() const { return changed_; }
    bool isChanged(TuneField field) const;
    ProcessingConfigPatch patch() const;
    QStringList validationIssues() const { return validateTuneConfig(draft_); }
    bool valid() const { return validationIssues().isEmpty(); }

    // A new authoritative config arrived (file reload, profile switch).
    //  - no local edits: it becomes the baseline (Refreshed), or Unchanged
    //    when no exposed field differs;
    //  - local edits and an exposed field differs from the baseline: the
    //    draft is retained and the panel is in Conflict until Revert;
    //  - during an in-flight apply the value is parked (Deferred) and
    //    reconciled by completeApply().
    ExternalOutcome noteExternalBaseline(const backend::services::ProcessingConfig& cfg);
    bool conflict() const { return conflict_; }
    // Drop local edits and adopt the latest authoritative config.
    void revert();

    // Apply lifecycle. beginApply() returns 0 (and does nothing) when the
    // draft is clean, invalid, in conflict or already applying.
    uint64_t beginApply();
    bool applying() const { return applying_; }
    uint64_t activeRequest() const { return activeRequest_; }
    // Result for the active request (a stale request id is ignored, false).
    bool completeApply(const ConfigApplyResult& result);
    // Local failure (no coordinator, validation) for the active request.
    void failApply(const QString& error);
    QString lastError() const { return lastError_; }
    bool savedNotApplied() const { return savedNotApplied_; }

    // "Applied", "3 unapplied changes", "Conflict: ...", "Applying…", ...
    QString stateText() const;

private:
    void markChanged(TuneField field, bool changed);
    bool exposedFieldsDiffer(const backend::services::ProcessingConfig& a, const backend::services::ProcessingConfig& b) const;

    backend::services::ProcessingConfig baseline_{};
    backend::services::ProcessingConfig draft_{};
    backend::services::ProcessingConfig pending_{};
    bool hasPending_{false};
    std::vector<TuneField> changed_;
    bool conflict_{false};
    bool applying_{false};
    bool savedNotApplied_{false};
    uint64_t activeRequest_{0};
    uint64_t nextRequest_{1};
    QString lastError_;
};

} // namespace frontend

Q_DECLARE_METATYPE(frontend::ApplyProcessingDraftRequest)
Q_DECLARE_METATYPE(frontend::ConfigApplyResult)
