// processing_config_draft_test (issue #364)
//
// Pure model semantics for the Monitoring tune panel:
//  - every exposed field maps to exactly one ProcessingConfig member and one
//    JSON path, with a unique full label and the right group;
//  - a patch carries only deliberately changed fields; an untouched
//    high-precision value is never rewritten because a control displays fewer
//    decimals; typing the displayed value back is not an edit;
//  - applyTunePatchToJson writes only the patched keys and preserves unknown
//    keys / other sections;
//  - validation is field-local and only for enabled ranges;
//  - external baselines: refresh when clean, ignore unrelated changes when
//    dirty, conflict (draft retained) when an exposed field differs, deferred
//    during an apply; Revert adopts the latest authoritative config;
//  - apply lifecycle: request ids, stale results ignored, Dirty clears only
//    on persisted && applied, saved-but-not-applied is explicit.

#include "frontend/models/ProcessingConfigDraft.h"

#include "support/assert.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

using backend::services::ProcessingConfig;
using namespace frontend;

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);

    // ---- 1. mapping -------------------------------------------------------
    {
        QSet<QString> labels, paths;
        int idx = 0;
        for (TuneField f : allTuneFields()) {
            MIB_EXPECT(!tuneFieldLabel(f).isEmpty(), "label present");
            MIB_EXPECT(!labels.contains(tuneFieldLabel(f)), "labels unique");
            labels.insert(tuneFieldLabel(f));
            MIB_EXPECT(tuneFieldJsonPath(f).startsWith(QStringLiteral("image_processing/")), "json path under image_processing");
            MIB_EXPECT(!paths.contains(tuneFieldJsonPath(f)), "json paths unique");
            paths.insert(tuneFieldJsonPath(f));
            // Round trip through the config member.
            ProcessingConfig c;
            const QVariant probe = tuneFieldIsBool(f) ? QVariant(!readTuneField(c, f).toBool())
                                 : tuneFieldDecimals(f) == 0 ? QVariant(1000 + idx)
                                                             : QVariant(0.25 + idx * 0.01);
            writeTuneField(c, f, probe);
            MIB_EXPECT(tuneValuesEqual(f, readTuneField(c, f), probe), "write/read round trip");
            // Only that member changed.
            int diffs = 0;
            for (TuneField g : allTuneFields())
                if (!tuneValuesEqual(g, readTuneField(c, g), readTuneField(ProcessingConfig{}, g))) ++diffs;
            MIB_EXPECT(diffs == 1 || (f == TuneField::MultiImageCount && diffs == 1), "one field maps to one member");
            const bool target = QString::fromLatin1(toString(f)).startsWith(QStringLiteral("Target"));
            MIB_EXPECT((tuneFieldGroup(f) == TuneGroup::TargetGroup) == target, "target-group fields grouped apart");
            MIB_EXPECT(!target || tuneFieldLabel(f).startsWith(QStringLiteral("Target group")), "target labels are unmistakable");
            ++idx;
        }
        MIB_EXPECT(allTuneFields().size() == 20, "all 20 exposed fields enumerated");
        MIB_EXPECT(tuneFieldUnit(TuneField::AreaMin) == QStringLiteral(" µm²") && tuneFieldUnit(TuneField::DeformabilityMin).isEmpty(), "units");
    }

    // ---- 2. precision + patch ---------------------------------------------
    {
        ProcessingConfig base;
        base.deformability_threshold_min = 0.123456; // 2-decimal control shows 0.12
        base.ring_ratio_max = 25.04;                 // 1-decimal control shows 25.0
        ProcessingConfigDraft d;
        d.loadBaseline(base);
        MIB_EXPECT(!d.dirty() && d.stateText() == QStringLiteral("Applied"), "clean after load");
        MIB_EXPECT(!d.setField(TuneField::DeformabilityMin, 0.12), "re-typing the displayed value is not an edit");
        MIB_EXPECT(!d.dirty() && d.draft().deformability_threshold_min == 0.123456, "exact baseline retained");
        MIB_EXPECT(d.setField(TuneField::DeformabilityMin, 0.13), "real edit");
        MIB_EXPECT(d.dirty() && d.changeCount() == 1 && d.isChanged(TuneField::DeformabilityMin), "one change");
        d.setField(TuneField::AreaMax, 500);
        MIB_EXPECT(d.changeCount() == 2 && d.stateText() == QStringLiteral("2 unapplied changes"), "two changes + text");
        const ProcessingConfigPatch p = d.patch();
        MIB_EXPECT(p.values.size() == 2 && p.values.count(TuneField::RingRatioMax) == 0, "patch carries only changed fields");
        ProcessingConfig target = base;
        target.gaussian_blur_size = 7; // unrelated field in the target
        const ProcessingConfig merged = applyTunePatch(target, p);
        MIB_EXPECT(merged.gaussian_blur_size == 7 && merged.ring_ratio_max == 25.04 && merged.area_threshold_max == 500
                       && merged.deformability_threshold_min == 0.13,
                   "merge touches only patched fields");
        MIB_EXPECT(tunePatchSatisfiedBy(merged, p) && !tunePatchSatisfiedBy(base, p), "satisfiedBy");
        d.setField(TuneField::DeformabilityMin, 0.12);
        MIB_EXPECT(d.changeCount() == 1 && d.draft().deformability_threshold_min == 0.123456, "editing back restores the exact value");
        d.setField(TuneField::AreaMax, base.area_threshold_max);
        MIB_EXPECT(!d.dirty(), "all edits undone -> clean");
    }

    // ---- 3. JSON patching preserves everything else ----------------------
    {
        const QByteArray json = R"({
          "buffer_threshold": 1000,
          "custom_top": {"keep": true},
          "image_processing": {
            "gaussian_blur_size": 5,
            "deformability_threshold_min": 0.123456,
            "area_threshold_min": 60,
            "filters": {"enable_border_check": true, "future_filter": "x"},
            "unknown_nested": {"a": [1,2,3]}
          }
        })";
        QJsonObject root = QJsonDocument::fromJson(json).object();
        ProcessingConfigPatch p;
        p.values[TuneField::AreaMin] = 300;
        p.values[TuneField::BorderEnabled] = false;
        p.values[TuneField::TargetAreaMax] = 999;   // target_group object missing -> created
        p.values[TuneField::MultiImageCount] = 4;  // multi_image missing -> created
        applyTunePatchToJson(root, p);
        const QJsonObject ip = root.value(QStringLiteral("image_processing")).toObject();
        MIB_EXPECT(ip.value(QStringLiteral("area_threshold_min")).toInt() == 300, "int key written");
        MIB_EXPECT(ip.value(QStringLiteral("filters")).toObject().value(QStringLiteral("enable_border_check")).toBool() == false, "bool key written");
        MIB_EXPECT(ip.value(QStringLiteral("filters")).toObject().value(QStringLiteral("future_filter")).toString() == QStringLiteral("x"), "sibling unknown key kept");
        MIB_EXPECT(ip.value(QStringLiteral("target_group")).toObject().value(QStringLiteral("area_max")).toInt() == 999, "missing object created");
        MIB_EXPECT(ip.value(QStringLiteral("multi_image")).toObject().value(QStringLiteral("count")).toInt() == 4, "multi_image created");
        MIB_EXPECT(ip.value(QStringLiteral("deformability_threshold_min")).toDouble() == 0.123456, "untouched high-precision value intact");
        MIB_EXPECT(ip.value(QStringLiteral("gaussian_blur_size")).toInt() == 5 && ip.contains(QStringLiteral("unknown_nested")), "other image_processing keys kept");
        MIB_EXPECT(root.value(QStringLiteral("buffer_threshold")).toInt() == 1000 && root.contains(QStringLiteral("custom_top")), "other sections kept");
    }

    // ---- 4. validation ------------------------------------------------------
    {
        ProcessingConfig c;
        c.area_threshold_min = 500;
        c.area_threshold_max = 100;
        c.enable_area_range_check = false;
        MIB_EXPECT(validateTuneConfig(c).isEmpty(), "disabled inverted range is not an error");
        c.enable_area_range_check = true;
        MIB_EXPECT(validateTuneConfig(c).size() == 1 && validateTuneConfig(c).first().startsWith(QStringLiteral("Area")), "enabled inverted range reported field-locally");
        c.multi_image_enabled = true;
        c.multi_image_count = 0;
        MIB_EXPECT(validateTuneConfig(c).size() == 2, "multi-image count validated");
        ProcessingConfigDraft d;
        d.loadBaseline(ProcessingConfig{});
        d.setField(TuneField::AreaMin, 100000);
        MIB_EXPECT(d.dirty() && !d.valid() && d.stateText().startsWith(QStringLiteral("Invalid")), "invalid draft text");
        MIB_EXPECT(d.beginApply() == 0, "invalid draft cannot be applied");
    }

    // ---- 5. external baselines -----------------------------------------------
    {
        ProcessingConfig base;
        ProcessingConfigDraft d;
        d.loadBaseline(base);
        ProcessingConfig ext = base;
        MIB_EXPECT(d.noteExternalBaseline(ext) == ProcessingConfigDraft::ExternalOutcome::Unchanged, "same values -> Unchanged");
        ext.ring_ratio_max = 30.0;
        MIB_EXPECT(d.noteExternalBaseline(ext) == ProcessingConfigDraft::ExternalOutcome::Refreshed && d.draft().ring_ratio_max == 30.0,
                   "clean panel refreshes to the external values");
        d.setField(TuneField::DeformabilityMax, 0.9);
        ProcessingConfig unrelated = ext;
        unrelated.gaussian_blur_size = 9; // not exposed
        MIB_EXPECT(d.noteExternalBaseline(unrelated) == ProcessingConfigDraft::ExternalOutcome::Unchanged && d.dirty() && !d.conflict(),
                   "unexposed change while dirty is not a conflict");
        MIB_EXPECT(d.baseline().gaussian_blur_size == 9 && d.draft().gaussian_blur_size == 9, "unexposed values follow the baseline");
        ProcessingConfig clash = unrelated;
        clash.area_threshold_max = 777;
        MIB_EXPECT(d.noteExternalBaseline(clash) == ProcessingConfigDraft::ExternalOutcome::Conflict && d.conflict(), "exposed change while dirty -> Conflict");
        MIB_EXPECT(d.draft().deformability_threshold_max == 0.9 && d.draft().area_threshold_max != 777, "draft retained in conflict");
        MIB_EXPECT(d.beginApply() == 0 && d.stateText().startsWith(QStringLiteral("Conflict")), "conflict blocks apply, explicit text");
        d.revert();
        MIB_EXPECT(!d.dirty() && !d.conflict() && d.draft().area_threshold_max == 777 && d.draft().deformability_threshold_max == unrelated.deformability_threshold_max,
                   "revert adopts the latest authoritative config");
    }

    // ---- 6. apply lifecycle --------------------------------------------------
    {
        ProcessingConfig base;
        ProcessingConfigDraft d;
        d.loadBaseline(base);
        MIB_EXPECT(d.beginApply() == 0, "clean draft: nothing to apply");
        d.setField(TuneField::AreaMin, 200);
        const uint64_t id = d.beginApply();
        MIB_EXPECT(id != 0 && d.applying() && d.stateText() == QStringLiteral("Applying…"), "apply started");
        MIB_EXPECT(!d.setField(TuneField::AreaMax, 999) && d.draft().area_threshold_max == base.area_threshold_max, "edits ignored mid-apply");
        MIB_EXPECT(d.beginApply() == 0, "single-flight");
        ProcessingConfig ext = base;
        ext.ring_ratio_min = 10.0;
        MIB_EXPECT(d.noteExternalBaseline(ext) == ProcessingConfigDraft::ExternalOutcome::Deferred, "external change parked during apply");
        ConfigApplyResult stale;
        stale.requestId = id + 5;
        stale.persisted = stale.applied = true;
        MIB_EXPECT(!d.completeApply(stale) && d.applying(), "stale result ignored");
        ConfigApplyResult ok;
        ok.requestId = id;
        ok.persisted = ok.applied = true;
        ok.effectiveConfig = applyTunePatch(base, d.patch());
        MIB_EXPECT(d.completeApply(ok) && !d.applying() && !d.dirty() && d.baseline().area_threshold_min == 200, "ok -> clean with effective baseline");
        MIB_EXPECT(d.baseline().ring_ratio_min == base.ring_ratio_min && !d.conflict() && !d.dirty(),
                   "a value parked during a successful apply is superseded by the effective config");
        ProcessingConfig later = ok.effectiveConfig;
        later.ring_ratio_min = 10.0;
        MIB_EXPECT(d.noteExternalBaseline(later) == ProcessingConfigDraft::ExternalOutcome::Refreshed && d.baseline().ring_ratio_min == 10.0,
                   "the next authoritative reload re-syncs cleanly");

        // Failure keeps Dirty.
        d.setField(TuneField::AreaMax, 900);
        const uint64_t id2 = d.beginApply();
        ConfigApplyResult fail;
        fail.requestId = id2;
        fail.error = QStringLiteral("disk full");
        d.completeApply(fail);
        MIB_EXPECT(d.dirty() && !d.applying() && d.stateText() == QStringLiteral("Not applied: disk full"), "failure keeps the draft and names the error");
        const uint64_t id3 = d.beginApply();
        MIB_EXPECT(id3 != 0 && id3 != id2, "retry gets a new request id");
        ConfigApplyResult partial;
        partial.requestId = id3;
        partial.persisted = true;
        partial.applied = false;
        d.completeApply(partial);
        MIB_EXPECT(d.dirty() && d.savedNotApplied() && d.stateText().startsWith(QStringLiteral("Saved, not applied")), "saved-but-not-applied is explicit and still dirty");
        const uint64_t id4 = d.beginApply();
        ConfigApplyResult conflict;
        conflict.requestId = id4;
        conflict.conflict = true;
        conflict.error = QStringLiteral("changed on disk");
        d.completeApply(conflict);
        MIB_EXPECT(d.conflict() && d.dirty() && d.beginApply() == 0, "conflict result -> Conflict state");
        d.failApply(QStringLiteral("x")); // no-op when not applying, harmless
        d.revert();
        MIB_EXPECT(!d.dirty() && !d.conflict() && d.stateText() == QStringLiteral("Applied"), "revert clears conflict");
    }

    return mib::test::exitCode();
}
