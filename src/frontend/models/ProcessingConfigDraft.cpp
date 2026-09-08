#include "frontend/models/ProcessingConfigDraft.h"

#include <QJsonValue>
#include <QObject>

#include <algorithm>
#include <cmath>
#include <functional>

namespace frontend {

using backend::services::ProcessingConfig;

namespace {

struct FieldInfo {
    TuneField field;
    const char* name;
    TuneGroup group;
    const char* label;   // full name (+ unit)
    const char* unit;    // control suffix
    int decimals;        // display precision (doubles)
    bool isBool;
    const char* jsonPath;
};

// Order = presentation order.
const FieldInfo kFields[] = {
    {TuneField::AreaEnabled, "AreaEnabled", TuneGroup::Acceptance, QT_TRANSLATE_NOOP("TuneField", "Area filter enabled"), "", 0, true, "image_processing/filters/enable_area_range_check"},
    {TuneField::AreaMin, "AreaMin", TuneGroup::Acceptance, QT_TRANSLATE_NOOP("TuneField", "Area minimum (µm²)"), " µm²", 0, false, "image_processing/area_threshold_min"},
    {TuneField::AreaMax, "AreaMax", TuneGroup::Acceptance, QT_TRANSLATE_NOOP("TuneField", "Area maximum (µm²)"), " µm²", 0, false, "image_processing/area_threshold_max"},
    {TuneField::DeformabilityEnabled, "DeformabilityEnabled", TuneGroup::Acceptance, QT_TRANSLATE_NOOP("TuneField", "Deformability filter enabled"), "", 0, true, "image_processing/filters/enable_deformability_range_check"},
    {TuneField::DeformabilityMin, "DeformabilityMin", TuneGroup::Acceptance, QT_TRANSLATE_NOOP("TuneField", "Deformability minimum"), "", 2, false, "image_processing/deformability_threshold_min"},
    {TuneField::DeformabilityMax, "DeformabilityMax", TuneGroup::Acceptance, QT_TRANSLATE_NOOP("TuneField", "Deformability maximum"), "", 2, false, "image_processing/deformability_threshold_max"},
    {TuneField::RingRatioEnabled, "RingRatioEnabled", TuneGroup::Acceptance, QT_TRANSLATE_NOOP("TuneField", "Ring ratio filter enabled"), "", 0, true, "image_processing/filters/enable_ring_ratio_check"},
    {TuneField::RingRatioMin, "RingRatioMin", TuneGroup::Acceptance, QT_TRANSLATE_NOOP("TuneField", "Ring ratio minimum"), "", 1, false, "image_processing/ring_ratio_min"},
    {TuneField::RingRatioMax, "RingRatioMax", TuneGroup::Acceptance, QT_TRANSLATE_NOOP("TuneField", "Ring ratio maximum"), "", 1, false, "image_processing/ring_ratio_max"},
    {TuneField::AreaRatioEnabled, "AreaRatioEnabled", TuneGroup::Acceptance, QT_TRANSLATE_NOOP("TuneField", "Area ratio filter enabled"), "", 0, true, "image_processing/filters/enable_area_ratio_check"},
    {TuneField::AreaRatioMax, "AreaRatioMax", TuneGroup::Acceptance, QT_TRANSLATE_NOOP("TuneField", "Area ratio maximum"), "", 2, false, "image_processing/area_ratio_threshold_max"},
    {TuneField::BorderEnabled, "BorderEnabled", TuneGroup::Acceptance, QT_TRANSLATE_NOOP("TuneField", "Border exclusion enabled"), "", 0, true, "image_processing/filters/enable_border_check"},
    {TuneField::SingleInnerContourEnabled, "SingleInnerContourEnabled", TuneGroup::Acceptance, QT_TRANSLATE_NOOP("TuneField", "Single inner contour required"), "", 0, true, "image_processing/filters/require_single_inner_contour"},
    {TuneField::TargetGroupEnabled, "TargetGroupEnabled", TuneGroup::TargetGroup, QT_TRANSLATE_NOOP("TuneField", "Target group sorting gate enabled"), "", 0, true, "image_processing/target_group/enabled"},
    {TuneField::TargetAreaMin, "TargetAreaMin", TuneGroup::TargetGroup, QT_TRANSLATE_NOOP("TuneField", "Target group area minimum (µm²)"), " µm²", 0, false, "image_processing/target_group/area_min"},
    {TuneField::TargetAreaMax, "TargetAreaMax", TuneGroup::TargetGroup, QT_TRANSLATE_NOOP("TuneField", "Target group area maximum (µm²)"), " µm²", 0, false, "image_processing/target_group/area_max"},
    {TuneField::TargetDeformabilityMin, "TargetDeformabilityMin", TuneGroup::TargetGroup, QT_TRANSLATE_NOOP("TuneField", "Target group deformability minimum"), "", 2, false, "image_processing/target_group/deformability_min"},
    {TuneField::TargetDeformabilityMax, "TargetDeformabilityMax", TuneGroup::TargetGroup, QT_TRANSLATE_NOOP("TuneField", "Target group deformability maximum"), "", 2, false, "image_processing/target_group/deformability_max"},
    {TuneField::MultiImageEnabled, "MultiImageEnabled", TuneGroup::MultiImage, QT_TRANSLATE_NOOP("TuneField", "Multi-image acquisition enabled"), "", 0, true, "image_processing/multi_image/enabled"},
    {TuneField::MultiImageCount, "MultiImageCount", TuneGroup::MultiImage, QT_TRANSLATE_NOOP("TuneField", "Images per trigger"), "", 0, false, "image_processing/multi_image/count"},
};

const FieldInfo& info(TuneField f)
{
    for (const auto& i : kFields)
        if (i.field == f) return i;
    return kFields[0];
}

} // namespace

const std::vector<TuneField>& allTuneFields()
{
    static const std::vector<TuneField> fields = [] {
        std::vector<TuneField> v;
        for (const auto& i : kFields) v.push_back(i.field);
        return v;
    }();
    return fields;
}

const char* toString(TuneField field) { return info(field).name; }
TuneGroup tuneFieldGroup(TuneField field) { return info(field).group; }
QString tuneFieldLabel(TuneField field) { return QObject::tr(info(field).label); }
QString tuneFieldUnit(TuneField field) { return QString::fromUtf8(info(field).unit); }
int tuneFieldDecimals(TuneField field) { return info(field).decimals; }
bool tuneFieldIsBool(TuneField field) { return info(field).isBool; }
QString tuneFieldJsonPath(TuneField field) { return QString::fromLatin1(info(field).jsonPath); }

QVariant readTuneField(const ProcessingConfig& c, TuneField f)
{
    switch (f) {
    case TuneField::AreaEnabled: return c.enable_area_range_check;
    case TuneField::AreaMin: return c.area_threshold_min;
    case TuneField::AreaMax: return c.area_threshold_max;
    case TuneField::DeformabilityEnabled: return c.enable_deformability_range_check;
    case TuneField::DeformabilityMin: return c.deformability_threshold_min;
    case TuneField::DeformabilityMax: return c.deformability_threshold_max;
    case TuneField::RingRatioEnabled: return c.enable_ring_ratio_check;
    case TuneField::RingRatioMin: return c.ring_ratio_min;
    case TuneField::RingRatioMax: return c.ring_ratio_max;
    case TuneField::AreaRatioEnabled: return c.enable_area_ratio_check;
    case TuneField::AreaRatioMax: return c.area_ratio_threshold_max;
    case TuneField::BorderEnabled: return c.enable_border_check;
    case TuneField::SingleInnerContourEnabled: return c.require_single_inner_contour;
    case TuneField::TargetGroupEnabled: return c.enable_target_group;
    case TuneField::TargetAreaMin: return c.target_group_area_min;
    case TuneField::TargetAreaMax: return c.target_group_area_max;
    case TuneField::TargetDeformabilityMin: return c.target_group_deformability_min;
    case TuneField::TargetDeformabilityMax: return c.target_group_deformability_max;
    case TuneField::MultiImageEnabled: return c.multi_image_enabled;
    case TuneField::MultiImageCount: return c.multi_image_count;
    }
    return {};
}

void writeTuneField(ProcessingConfig& c, TuneField f, const QVariant& v)
{
    switch (f) {
    case TuneField::AreaEnabled: c.enable_area_range_check = v.toBool(); break;
    case TuneField::AreaMin: c.area_threshold_min = v.toInt(); break;
    case TuneField::AreaMax: c.area_threshold_max = v.toInt(); break;
    case TuneField::DeformabilityEnabled: c.enable_deformability_range_check = v.toBool(); break;
    case TuneField::DeformabilityMin: c.deformability_threshold_min = v.toDouble(); break;
    case TuneField::DeformabilityMax: c.deformability_threshold_max = v.toDouble(); break;
    case TuneField::RingRatioEnabled: c.enable_ring_ratio_check = v.toBool(); break;
    case TuneField::RingRatioMin: c.ring_ratio_min = v.toDouble(); break;
    case TuneField::RingRatioMax: c.ring_ratio_max = v.toDouble(); break;
    case TuneField::AreaRatioEnabled: c.enable_area_ratio_check = v.toBool(); break;
    case TuneField::AreaRatioMax: c.area_ratio_threshold_max = v.toDouble(); break;
    case TuneField::BorderEnabled: c.enable_border_check = v.toBool(); break;
    case TuneField::SingleInnerContourEnabled: c.require_single_inner_contour = v.toBool(); break;
    case TuneField::TargetGroupEnabled: c.enable_target_group = v.toBool(); break;
    case TuneField::TargetAreaMin: c.target_group_area_min = v.toInt(); break;
    case TuneField::TargetAreaMax: c.target_group_area_max = v.toInt(); break;
    case TuneField::TargetDeformabilityMin: c.target_group_deformability_min = v.toDouble(); break;
    case TuneField::TargetDeformabilityMax: c.target_group_deformability_max = v.toDouble(); break;
    case TuneField::MultiImageEnabled: c.multi_image_enabled = v.toBool(); break;
    case TuneField::MultiImageCount: c.multi_image_count = std::max(1, v.toInt()); break;
    }
}

bool tuneValuesEqual(TuneField f, const QVariant& a, const QVariant& b)
{
    if (tuneFieldIsBool(f)) return a.toBool() == b.toBool();
    const int decimals = tuneFieldDecimals(f);
    if (decimals == 0) return a.toInt() == b.toInt();
    const double scale = std::pow(10.0, decimals);
    return std::llround(a.toDouble() * scale) == std::llround(b.toDouble() * scale);
}

ProcessingConfig applyTunePatch(ProcessingConfig target, const ProcessingConfigPatch& patch)
{
    for (const auto& [field, value] : patch.values) writeTuneField(target, field, value);
    return target;
}

void applyTunePatchToJson(QJsonObject& root, const ProcessingConfigPatch& patch)
{
    for (const auto& [field, value] : patch.values) {
        const QStringList parts = tuneFieldJsonPath(field).split(QLatin1Char('/'));
        // Recursive replace along the path; intermediate objects are created
        // when missing and every sibling key is kept.
        std::function<QJsonObject(QJsonObject, int)> set = [&](QJsonObject obj, int depth) {
            const QString& key = parts[depth];
            if (depth == parts.size() - 1) {
                if (tuneFieldIsBool(field)) obj.insert(key, value.toBool());
                else if (tuneFieldDecimals(field) == 0) obj.insert(key, value.toInt());
                else obj.insert(key, value.toDouble());
                return obj;
            }
            QJsonObject child = obj.value(key).isObject() ? obj.value(key).toObject() : QJsonObject();
            obj.insert(key, set(child, depth + 1));
            return obj;
        };
        root = set(root, 0);
    }
}

bool tunePatchSatisfiedBy(const ProcessingConfig& cfg, const ProcessingConfigPatch& patch)
{
    for (const auto& [field, value] : patch.values)
        if (!tuneValuesEqual(field, readTuneField(cfg, field), value)) return false;
    return true;
}

QStringList validateTuneConfig(const ProcessingConfig& c)
{
    QStringList issues;
    if (c.enable_area_range_check && c.area_threshold_min > c.area_threshold_max)
        issues << QObject::tr("Area: minimum (%1 µm²) exceeds maximum (%2 µm²)").arg(c.area_threshold_min).arg(c.area_threshold_max);
    if (c.enable_deformability_range_check && c.deformability_threshold_min > c.deformability_threshold_max)
        issues << QObject::tr("Deformability: minimum exceeds maximum");
    if (c.enable_ring_ratio_check && c.ring_ratio_min > c.ring_ratio_max)
        issues << QObject::tr("Ring ratio: minimum exceeds maximum");
    if (c.enable_target_group) {
        if (c.target_group_area_min > c.target_group_area_max)
            issues << QObject::tr("Target group: area minimum exceeds maximum");
        if (c.target_group_deformability_min > c.target_group_deformability_max)
            issues << QObject::tr("Target group: deformability minimum exceeds maximum");
    }
    if (c.multi_image_enabled && c.multi_image_count < 1)
        issues << QObject::tr("Multi-image: at least one image per trigger is required");
    return issues;
}

// ---------------------------------------------------------------------------

void ProcessingConfigDraft::loadBaseline(const ProcessingConfig& cfg)
{
    baseline_ = cfg;
    draft_ = cfg;
    changed_.clear();
    conflict_ = false;
    hasPending_ = false;
    savedNotApplied_ = false;
    lastError_.clear();
}

bool ProcessingConfigDraft::isChanged(TuneField field) const
{
    return std::find(changed_.begin(), changed_.end(), field) != changed_.end();
}

void ProcessingConfigDraft::markChanged(TuneField field, bool changed)
{
    const auto it = std::find(changed_.begin(), changed_.end(), field);
    if (changed && it == changed_.end()) changed_.push_back(field);
    else if (!changed && it != changed_.end()) changed_.erase(it);
}

bool ProcessingConfigDraft::setField(TuneField field, const QVariant& value)
{
    if (applying_) return false; // edits are not accepted mid-apply
    const bool wasDirty = dirty();
    const bool wasChanged = isChanged(field);
    if (tuneValuesEqual(field, value, baselineField(field))) {
        // Back to what the operator sees as the baseline: restore the exact
        // baseline value (never round-trip a rounded copy).
        writeTuneField(draft_, field, baselineField(field));
        markChanged(field, false);
    } else {
        writeTuneField(draft_, field, value);
        markChanged(field, true);
    }
    lastError_.clear();
    return wasDirty != dirty() || wasChanged != isChanged(field);
}

ProcessingConfigPatch ProcessingConfigDraft::patch() const
{
    ProcessingConfigPatch p;
    for (TuneField f : changed_) p.values[f] = readTuneField(draft_, f);
    return p;
}

bool ProcessingConfigDraft::exposedFieldsDiffer(const ProcessingConfig& a, const ProcessingConfig& b) const
{
    for (TuneField f : allTuneFields())
        if (!tuneValuesEqual(f, readTuneField(a, f), readTuneField(b, f))) return true;
    return false;
}

ProcessingConfigDraft::ExternalOutcome ProcessingConfigDraft::noteExternalBaseline(const ProcessingConfig& cfg)
{
    if (applying_) {
        pending_ = cfg;
        hasPending_ = true;
        return ExternalOutcome::Deferred;
    }
    if (!dirty()) {
        const bool differs = exposedFieldsDiffer(cfg, baseline_) || conflict_;
        loadBaseline(cfg);
        return differs ? ExternalOutcome::Refreshed : ExternalOutcome::Unchanged;
    }
    if (!exposedFieldsDiffer(cfg, baseline_)) {
        // Unrelated keys changed (or an echo): keep editing against the same
        // exposed values; unexposed members follow the new baseline.
        baseline_ = cfg;
        ProcessingConfig next = cfg;
        for (TuneField f : changed_) writeTuneField(next, f, readTuneField(draft_, f));
        draft_ = next;
        return ExternalOutcome::Unchanged;
    }
    pending_ = cfg;
    hasPending_ = true;
    conflict_ = true;
    return ExternalOutcome::Conflict;
}

void ProcessingConfigDraft::revert()
{
    if (applying_) return;
    loadBaseline(hasPending_ ? pending_ : baseline_);
}

uint64_t ProcessingConfigDraft::beginApply()
{
    if (!dirty() || conflict_ || applying_ || !valid()) return 0;
    applying_ = true;
    savedNotApplied_ = false;
    lastError_.clear();
    activeRequest_ = nextRequest_++;
    return activeRequest_;
}

bool ProcessingConfigDraft::completeApply(const ConfigApplyResult& result)
{
    if (!applying_ || result.requestId != activeRequest_) return false;
    applying_ = false;
    activeRequest_ = 0;
    if (result.ok()) {
        // The effective config is newer than anything parked while applying
        // (the coordinator refuses to write over an unprocessed external
        // change); the next authoritative reload re-syncs the panel.
        loadBaseline(result.effectiveConfig);
        return true;
    }
    // Failure / partial outcome: keep the draft (still dirty), record why.
    if (result.persisted && !result.applied) {
        savedNotApplied_ = true;
        lastError_ = result.error.isEmpty() ? QObject::tr("Saved to the configuration file but not applied by the processing service") : result.error;
    } else {
        lastError_ = result.error.isEmpty() ? QObject::tr("Apply failed") : result.error;
    }
    if (result.conflict) conflict_ = true;
    if (hasPending_) {
        const ProcessingConfig p = pending_;
        hasPending_ = false;
        noteExternalBaseline(p);
    }
    return true;
}

void ProcessingConfigDraft::failApply(const QString& error)
{
    applying_ = false;
    activeRequest_ = 0;
    lastError_ = error;
}

QString ProcessingConfigDraft::stateText() const
{
    if (applying_) return QObject::tr("Applying…");
    if (conflict_) return QObject::tr("Conflict: the configuration changed elsewhere. Revert to load it, or keep editing and Apply after reverting.");
    if (savedNotApplied_) return QObject::tr("Saved, not applied: %1").arg(lastError_);
    if (!lastError_.isEmpty()) return QObject::tr("Not applied: %1").arg(lastError_);
    const QStringList issues = validationIssues();
    if (dirty() && !issues.isEmpty()) return QObject::tr("Invalid: %1").arg(issues.first());
    if (dirty()) return changeCount() == 1 ? QObject::tr("1 unapplied change") : QObject::tr("%1 unapplied changes").arg(changeCount());
    return QObject::tr("Applied");
}

} // namespace frontend
