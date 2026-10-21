// calibration_record_test (#716 A4a, bridge ABI 42): the calibration run's pure parts.
//
//  - The method document's purpose / calibration_policy / reference: legacy defaults when absent,
//    strict refusal of anything malformed.
//  - Derivation on synthetic bead runs: a good run proposes nominal / median px; too few objects
//    and a high CV are `insufficient`; a drifted factor is reported against the tolerance; focus
//    and E are never fabricated; the event rate needs the file's declared clock.
//  - The `calibration.record` gate per policy (none / warn / required), age (soft), identity and
//    factor changes, and its Fix text.
//  - The store: append-only versions, explicit compare-and-set apply and every refusal (stale
//    expected factor, insufficient, not found, already applied, identity mismatch, unknown
//    identity, store unreadable, write fault), supersede, hard invalidation on identity change,
//    persistence across a reload, and no change from listing or reading.

#include "backend/app/CalibrationRecord.h"
#include "backend/app/CalibrationStore.h"

#include "support/assert.h"
#include "support/tempdir.h"

#include <cmath>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace backend::app;
namespace fs = std::filesystem;

namespace {

constexpr double kPi = 3.14159265358979323846;

// `n` beads of diameter `diameterPx` with a relative spread `cv` (normal), one per frame, frames
// `framePeriodTicks` apart on a clock of `tps` ticks per second.
CalibrationEvidence beads(std::size_t n, double diameterPx, double cv, uint64_t tps = 1'000'000,
                          uint64_t framePeriodTicks = 2000, unsigned seed = 7)
{
    CalibrationEvidence e;
    e.ticksPerSecond = tps;
    std::mt19937 gen(seed);
    std::normal_distribution<double> d(diameterPx, diameterPx * cv);
    for (std::size_t i = 0; i < n; ++i) {
        CalibrationSample s;
        s.frameIndex = i;
        s.timestamp = 1'000'000 + i * framePeriodTicks;
        const double dia = std::max(1.0, d(gen));
        s.areaPx = kPi * dia * dia / 4.0;
        s.deformation = 0.01;
        e.objects.push_back(s);
    }
    return e;
}

CalibrationReference reference(double nominalUm = 7.32, double tolUm = 0.1)
{
    CalibrationReference r;
    r.nominalDiameterUm = nominalUm;
    r.diameterToleranceUm = tolUm;
    return r;
}

CalibrationIdentity identityA()
{
    CalibrationIdentity id;
    id.camera = "mock:frames";
    id.roi = "0,0,96,96";
    return id;
}

CalibrationRecord derivedRecord(const std::string& id, const CalibrationEvidence& e, double runFactor,
                                const CalibrationIdentity& identity, uint64_t nowMs)
{
    CalibrationRecord r;
    r.recordId = id;
    r.createdAtUnixMs = r.statusAtUnixMs = nowMs;
    r.file = "/data/" + id + ".h5";
    r.runId = "run-" + id;
    r.methodSource = "local";
    r.reference = reference();
    r.identity = identity;
    r.derivation = deriveCalibration(e, r.reference, runFactor);
    r.status = r.derivation.sufficient() ? CalibrationStatus::Proposed : CalibrationStatus::Insufficient;
    return r;
}

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

} // namespace

int main()
{
    // ---- method document -------------------------------------------------------------------
    {
        CalibrationMethodSpec s;
        std::string why;
        MIB_EXPECT(parseCalibrationMethodSpec("", s, &why) && s.purpose == MethodPurpose::Measurement &&
                       s.policy == CalibrationPolicy::None && s.maxAgeHours == 24.0 && !s.reference,
                   "no config: a measurement method with policy none, 24 h");
        MIB_EXPECT(parseCalibrationMethodSpec(R"({"image_processing":{}})", s, &why) &&
                       s.purpose == MethodPurpose::Measurement && s.policy == CalibrationPolicy::None,
                   "legacy method unchanged");
        MIB_EXPECT(parseCalibrationMethodSpec(R"({"calibration_policy":"required","calibration_max_age_hours":8})", s,
                                              &why) &&
                       s.policy == CalibrationPolicy::Required && s.maxAgeHours == 8.0,
                   "a measurement method with a required policy: " + why);
        MIB_EXPECT(parseCalibrationMethodSpec(
                       R"({"purpose":"calibration","calibration_reference":{"nominal_diameter_um":7.32,"diameter_tolerance_um":0.1,"nominal_e_kpa":2000,"max_cv_percent":6}})",
                       s, &why) &&
                       s.purpose == MethodPurpose::Calibration && s.reference &&
                       s.reference->nominalDiameterUm == 7.32 && s.reference->nominalEKpa == 2000.0 &&
                       s.reference->maxCvPercent == 6.0,
                   "a calibration method with its reference: " + why);
        MIB_EXPECT(parseCalibrationMethodSpec(
                       R"({"purpose":"calibration","calibration_reference":{"nominal_diameter_um":5,"diameter_tolerance_um":0.2}})",
                       s, &why) &&
                       s.reference && s.reference->maxCvPercent == kCalibrationDefaultMaxCvPercent &&
                       !s.reference->nominalEKpa,
                   "default CV limit 10 %, no nominal E");
        auto refused = [&](const char* text, const char* key) {
            CalibrationMethodSpec x;
            std::string w;
            const bool ok = parseCalibrationMethodSpec(text, x, &w);
            return !ok && w.find(key) != std::string::npos;
        };
        MIB_EXPECT(refused(R"({"purpose":"qc"})", "purpose"), "unknown purpose refused");
        MIB_EXPECT(refused(R"({"purpose":3})", "purpose"), "purpose type");
        MIB_EXPECT(refused(R"({"calibration_policy":"always"})", "calibration_policy"), "unknown policy refused");
        MIB_EXPECT(refused(R"({"calibration_max_age_hours":0})", "calibration_max_age_hours"), "age must be > 0");
        MIB_EXPECT(refused(R"({"purpose":"calibration"})", "calibration_reference"), "a calibration method needs a reference");
        MIB_EXPECT(refused(R"({"calibration_reference":{"nominal_diameter_um":7,"diameter_tolerance_um":0.1}})",
                           "calibration_reference"),
                   "a reference on a measurement method is refused");
        MIB_EXPECT(refused(R"({"purpose":"calibration","calibration_reference":{"nominal_diameter_um":-1,"diameter_tolerance_um":0.1}})",
                           "nominal_diameter_um"),
                   "nominal must be > 0");
        MIB_EXPECT(refused(R"({"purpose":"calibration","calibration_reference":{"nominal_diameter_um":7}})",
                           "diameter_tolerance_um"),
                   "the tolerance is required");
        MIB_EXPECT(refused(R"({"purpose":"calibration","calibration_reference":{"nominal_diameter_um":7,"diameter_tolerance_um":0.1,"max_cv_percent":150}})",
                           "max_cv_percent"),
                   "CV limit within (0, 100]");
        MIB_EXPECT(refused(R"({"purpose":"calibration","calibration_reference":{"nominal_diameter_um":7,"diameter_tolerance_um":0.1,"offset":1}})",
                           "offset"),
                   "unknown reference member refused");
    }

    // ---- derivation: synthetic bead runs ----------------------------------------------------
    {
        // Good run: 400 beads of 15 px, CV ~3 %; the run used 0.4886 um/px.
        const auto good = deriveCalibration(beads(400, 15.0, 0.03), reference(), 0.4886);
        const auto& r = good.results;
        MIB_EXPECT(good.sufficient(), "a good bead run passes both gates");
        MIB_EXPECT(r.validObjects == 400 && r.excludedObjects == 0, "every bead counted");
        MIB_REQUIRE(r.medianDiameterPx && r.proposedPixelToMicron && r.cvDiameterPercent && r.madDiameterPx,
                    "median, MAD, CV and the proposal are computed");
        MIB_EXPECT(near(*r.medianDiameterPx, 15.0, 0.2), "median diameter ~15 px");
        MIB_EXPECT(near(*r.proposedPixelToMicron, 7.32 / *r.medianDiameterPx, 1e-12), "proposal = nominal / median");
        MIB_EXPECT(*r.cvDiameterPercent > 1.0 && *r.cvDiameterPercent < 5.0, "CV ~3 %");
        MIB_EXPECT(r.deformationMedian && near(*r.deformationMedian, 0.01, 1e-12), "deformation offset reported");
        MIB_EXPECT(r.focusStatus == "not recorded" && !r.focusLaplacianMedian,
                   "no focus column in the file: focus not recorded, never fabricated");
        MIB_EXPECT(r.eStatus == "not declared" && !r.eMedianKpa, "no nominal E: not compared");

        // Drifted factor: the beads measure 15 px, so at 0.52 um/px they read 7.8 um, outside
        // 7.32 +- 0.1; the proposal is the same nominal / median.
        const auto drifted = deriveCalibration(beads(400, 15.0, 0.03), reference(), 0.52);
        MIB_EXPECT(drifted.sufficient(), "a drifted factor still gives a sufficient record");
        MIB_EXPECT(drifted.results.runFactorWithinTolerance == std::optional<bool>(false) &&
                       drifted.results.measuredDiameterUmAtRunFactor &&
                       *drifted.results.measuredDiameterUmAtRunFactor > 7.6,
                   "the run's factor is reported outside the tolerance");
        MIB_EXPECT(good.results.runFactorWithinTolerance == std::optional<bool>(true),
                   "0.4886 um/px x 15 px = 7.33 um, within 7.32 +- 0.1");
        MIB_EXPECT(near(*drifted.results.proposedPixelToMicron, *good.results.proposedPixelToMicron, 1e-12),
                   "the proposal does not depend on the run's factor");

        // Too few objects.
        const auto few = deriveCalibration(beads(150, 15.0, 0.03), reference(), 0.4886);
        MIB_EXPECT(!few.sufficient(), "150 beads: insufficient");
        MIB_EXPECT(!few.gates[0].pass && few.gates[0].id == "min_valid_objects" && few.gates[0].value == 150.0 &&
                       few.gates[0].limit == 200.0 && few.gates[1].pass,
                   "min_valid_objects fails, CV passes");
        MIB_EXPECT(few.results.proposedPixelToMicron.has_value(),
                   "the measurement is still reported (but the record can never be applied)");
        MIB_EXPECT(deriveCalibration(beads(200, 15.0, 0.03), reference(), 0.4886).sufficient(), "exactly 200 is enough");

        // High CV (a polydisperse sample, debris).
        const auto wide = deriveCalibration(beads(400, 15.0, 0.25), reference(), 0.4886);
        MIB_EXPECT(!wide.sufficient() && wide.gates[0].pass && !wide.gates[1].pass &&
                       wide.gates[1].id == "max_cv_percent" && wide.gates[1].limit == 10.0,
                   "CV ~25 % fails the default 10 % limit");
        auto loose = reference();
        loose.maxCvPercent = 40.0;
        MIB_EXPECT(deriveCalibration(beads(400, 15.0, 0.25), loose, 0.4886).sufficient(),
                   "the method's own CV limit is honoured");

        // Nothing at all, and unusable areas.
        auto empty = deriveCalibration(CalibrationEvidence{}, reference(), 0.4886);
        MIB_EXPECT(!empty.sufficient() && !empty.results.medianDiameterPx && !empty.results.proposedPixelToMicron,
                   "no objects: insufficient, nothing proposed");
        auto withBad = beads(250, 15.0, 0.03);
        withBad.objects[3].areaPx = std::nan("");
        withBad.objects[4].areaPx = 0.0;
        withBad.objects[5].areaPx = -5.0;
        const auto bad = deriveCalibration(withBad, reference(), 0.4886);
        MIB_EXPECT(bad.results.validObjects == 247 && bad.results.excludedObjects == 3, "unusable areas excluded, counted");

        // Focus and E only where recorded; E only where declared.
        auto focused = beads(300, 15.0, 0.03);
        focused.focusFieldRecorded = true;
        focused.eFieldRecorded = true;
        for (std::size_t i = 0; i < focused.objects.size(); ++i) {
            focused.objects[i].laplacianVariance = 100.0 + static_cast<double>(i % 3);
            focused.objects[i].youngsModulusKpa = 1800.0;
        }
        auto withE = reference();
        withE.nominalEKpa = 2000.0;
        const auto fe = deriveCalibration(focused, withE, 0.4886);
        MIB_EXPECT(fe.results.focusStatus == "recorded" && fe.results.focusLaplacianMedian == 101.0 &&
                       fe.results.focusObjects == 300,
                   "a recorded focus metric is reported");
        MIB_EXPECT(fe.results.eStatus == "compared" && fe.results.eMedianKpa == 1800.0 &&
                       near(*fe.results.eRatio, 0.9, 1e-12) && fe.results.eObjects == 300,
                   "E against its nominal, reported only");
        auto noE = focused;
        for (auto& o : noE.objects) o.youngsModulusKpa = 0.0; // no LUT loaded: 0 is not an E
        MIB_EXPECT(deriveCalibration(noE, withE, 0.4886).results.eStatus == "not recorded",
                   "a file without E: not recorded, never 0 kPa");
        auto columnOnly = beads(300, 15.0, 0.03);
        columnOnly.focusFieldRecorded = true; // the column exists but every value is NaN (Contract 1)
        MIB_EXPECT(deriveCalibration(columnOnly, reference(), 0.4886).results.focusStatus == "not recorded",
                   "a focus column without values: not recorded");

        // Event rate: one object per frame every 2 ms over a 1 MHz clock -> 500/s over 400 frames
        // is only 0.8 s: too short; 2000 frames span 4 s: computed and stable.
        const auto shortRun = deriveCalibration(beads(400, 15.0, 0.03), reference(), 0.4886);
        MIB_EXPECT(shortRun.results.eventRateStatus == "too short", "0.8 s: too short for three windows");
        const auto longRun = deriveCalibration(beads(2000, 15.0, 0.03), reference(), 0.4886);
        MIB_EXPECT(longRun.results.eventRateStatus == "computed" && longRun.results.eventRateWindows == 3 &&
                       longRun.results.eventRateMeanPerS && near(*longRun.results.eventRateMeanPerS, 500.0, 1.0) &&
                       longRun.results.eventRateCvPercent && *longRun.results.eventRateCvPercent < 1.0,
                   "a steady 500/s over 3 windows");
        auto noClock = beads(2000, 15.0, 0.03);
        noClock.ticksPerSecond = 0;
        MIB_EXPECT(deriveCalibration(noClock, reference(), 0.4886).results.eventRateStatus == "not recorded",
                   "no declared timestamp clock: the event rate is not recorded");
        auto backwards = beads(2000, 15.0, 0.03);
        backwards.objects[10].timestamp = 0;
        MIB_EXPECT(deriveCalibration(backwards, reference(), 0.4886).results.eventRateStatus == "not monotonic",
                   "timestamps going back: not computed");
        MIB_EXPECT(deriveCalibration(beads(400, 15.0, 0.03), reference(), 0.0).results.runPixelToMicron == std::nullopt,
                   "an unknown run factor is not reported as 0");
    }

    // ---- record JSON round trip ------------------------------------------------------------
    {
        auto e = beads(300, 15.0, 0.03);
        e.focusFieldRecorded = true;
        for (auto& o : e.objects) o.laplacianVariance = 50.0;
        auto rec = derivedRecord("cal-1", e, 0.4886, identityA(), 1'760'000'000'000ull);
        rec.appliedPreviousFactor = 0.4886;
        rec.appliedNewFactor = 0.49;
        rec.status = CalibrationStatus::Applied;
        rec.version = 2;
        rec.sequence = 9;
        CalibrationRecord back;
        std::string why;
        MIB_REQUIRE(calibrationRecordFromJson(calibrationRecordToJson(rec), back, &why), "round trip parses: " + why);
        MIB_EXPECT(calibrationRecordToJson(back) == calibrationRecordToJson(rec), "the round trip is exact");
        MIB_EXPECT(back.identity == rec.identity && back.status == CalibrationStatus::Applied && back.version == 2 &&
                       back.appliedNewFactor == 0.49 && back.derivation.results.focusStatus == "recorded",
                   "members survive");
        MIB_EXPECT(!calibrationRecordFromJson(R"({"schema_version":1,"record_id":"x","status":"approved"})", back, &why),
                   "an unknown status is refused");
        MIB_EXPECT(newCalibrationRecordId(1'760'000'000'000ull).rfind("cal-20251009T", 0) == 0 &&
                       newCalibrationRecordId(1'760'000'000'000ull).size() <= kCalibrationRecordIdMaxBytes,
                   "record ids are dated and bounded");
    }

    // ---- the readiness gate ----------------------------------------------------------------
    {
        const uint64_t t0 = 1'760'000'000'000ull;
        auto applied = derivedRecord("cal-A", beads(300, 15.0, 0.03), 0.4886, identityA(), t0);
        applied.status = CalibrationStatus::Applied;
        applied.appliedPreviousFactor = 0.4886;
        applied.appliedNewFactor = 0.488;
        CalibrationGateInput in;
        in.current = identityA();
        in.currentFactor = 0.488;
        in.nowUnixMs = t0 + 3600'000ull; // 1 h later

        auto run = [&](CalibrationPolicy p, MethodPurpose purpose = MethodPurpose::Measurement) {
            auto x = in;
            x.spec.policy = p;
            x.spec.purpose = purpose;
            return evaluateCalibrationGate(x);
        };
        auto none = run(CalibrationPolicy::None);
        MIB_EXPECT(none.gate.id == "calibration.record" && none.gate.status == GateStatus::NotRequired &&
                       !none.inEffectRecordId,
                   "policy none: not required; nothing applied, nothing in effect");
        auto warnNone = run(CalibrationPolicy::Warn);
        MIB_EXPECT(warnNone.gate.status == GateStatus::Warn && warnNone.gate.remediation == "run the calibration method",
                   "warn without a record: Warn, Fix names the calibration method");
        auto reqNone = run(CalibrationPolicy::Required);
        MIB_EXPECT(reqNone.gate.status == GateStatus::Fail && reqNone.gate.blocksStart(),
                   "required without a record blocks Start");
        auto proposed = derivedRecord("cal-P", beads(300, 15.0, 0.03), 0.4886, identityA(), t0);
        in.proposed = {proposed};
        auto reqProposed = run(CalibrationPolicy::Required);
        MIB_EXPECT(reqProposed.gate.status == GateStatus::Fail &&
                       reqProposed.gate.remediation.find("apply proposed record cal-P") == 0,
                   "the Fix names the proposed record to apply");
        auto otherSetup = proposed;
        otherSetup.identity.roi = "0,0,64,64";
        in.proposed = {otherSetup};
        MIB_EXPECT(run(CalibrationPolicy::Required).gate.remediation == "run the calibration method",
                   "a proposal of another setup is not offered");
        in.proposed.clear();

        in.applied = applied;
        auto pass = run(CalibrationPolicy::Required);
        MIB_EXPECT(pass.gate.status == GateStatus::Pass && pass.inEffectRecordId == std::optional<std::string>("cal-A"),
                   "a fresh applied record in effect passes");
        MIB_EXPECT(run(CalibrationPolicy::None).inEffectRecordId == std::optional<std::string>("cal-A"),
                   "the in-effect record is known whatever the policy (the snapshot records it)");
        MIB_EXPECT(run(CalibrationPolicy::Required, MethodPurpose::Calibration).gate.status == GateStatus::NotRequired,
                   "a calibration method needs no applied record");

        auto old = in;
        old.spec.policy = CalibrationPolicy::Required;
        old.nowUnixMs = t0 + 25ull * 3600'000ull;
        const auto aged = evaluateCalibrationGate(old);
        MIB_EXPECT(aged.gate.status == GateStatus::Warn && !aged.gate.blocksStart() &&
                       aged.inEffectRecordId == std::optional<std::string>("cal-A") &&
                       aged.gate.remediation == "run the calibration method",
                   "age is soft: a 25 h old record warns, even when required");
        old.spec.maxAgeHours = 48;
        MIB_EXPECT(evaluateCalibrationGate(old).gate.status == GateStatus::Pass, "the method's max age is honoured");
        MIB_EXPECT(aged.invalidationKey != pass.invalidationKey, "expiry changes the readiness key");
        auto behind = in;
        behind.spec.policy = CalibrationPolicy::Warn;
        behind.nowUnixMs = t0 - 1000;
        MIB_EXPECT(evaluateCalibrationGate(behind).gate.status == GateStatus::Warn, "a clock behind the apply time warns");

        auto changedFactor = in;
        changedFactor.spec.policy = CalibrationPolicy::Required;
        changedFactor.currentFactor = 0.5;
        const auto cf = evaluateCalibrationGate(changedFactor);
        MIB_EXPECT(cf.gate.status == GateStatus::Fail && !cf.inEffectRecordId &&
                       cf.gate.remediation.find("re-apply record cal-A") == 0,
                   "a factor changed after the apply: the record is not in effect, Fix re-applies it");

        auto unknown = in;
        unknown.spec.policy = CalibrationPolicy::Required;
        unknown.current = CalibrationIdentity{};
        const auto uk = evaluateCalibrationGate(unknown);
        MIB_EXPECT(uk.gate.status == GateStatus::Unavailable && !uk.inEffectRecordId,
                   "unknown identity: unavailable (never Pass), not in effect");
        unknown.spec.policy = CalibrationPolicy::Warn;
        MIB_EXPECT(evaluateCalibrationGate(unknown).gate.status == GateStatus::Warn, "... a warning under warn");

        auto broken = in;
        broken.spec.policy = CalibrationPolicy::Required;
        broken.storeError = "unreadable";
        MIB_EXPECT(evaluateCalibrationGate(broken).gate.status == GateStatus::Unavailable, "an unreadable store blocks required");
        auto malformed = in;
        malformed.specError = "purpose: bad";
        MIB_EXPECT(evaluateCalibrationGate(malformed).gate.status == GateStatus::Fail, "a malformed method fails closed");
    }

    // ---- the store -------------------------------------------------------------------------
    {
        mib::test::TempDir td("calibration_store");
        const auto file = td.path() / "instrument_calibration.json";
        const uint64_t t0 = 1'760'000'000'000ull;
        {
            CalibrationStore store(file);
            MIB_EXPECT(store.error().empty() && store.revision() == 0 && store.latestRecords().empty() && !store.applied(),
                       "an absent file is an empty store");
            std::string why;
            MIB_REQUIRE(store.addDerived(derivedRecord("cal-1", beads(300, 15.0, 0.03), 0.4886, identityA(), t0), &why),
                        "add a proposed record: " + why);
            MIB_REQUIRE(store.addDerived(derivedRecord("cal-few", beads(50, 15.0, 0.03), 0.4886, identityA(), t0 + 1), &why),
                        "add an insufficient record: " + why);
            MIB_EXPECT(store.latest("cal-few")->status == CalibrationStatus::Insufficient, "insufficient as derived");
            MIB_EXPECT(!store.addDerived(derivedRecord("cal-1", beads(300, 15.0, 0.03), 0.4886, identityA(), t0), &why),
                       "an id is never reused");

            // No-actuation: listing and reading change nothing.
            const auto rev = store.revision();
            for (int i = 0; i < 5; ++i) {
                (void)store.latestRecords();
                (void)store.latest("cal-1");
                (void)store.history("cal-1");
                (void)store.applied();
            }
            MIB_EXPECT(store.revision() == rev && !store.applied(), "listing and reading never apply anything");
            MIB_EXPECT(!store.invalidateOnIdentityChange(identityA(), t0) && store.revision() == rev,
                       "an unchanged identity appends nothing");

            const double proposal = *store.latest("cal-1")->proposedFactor();
            // Refusals: nothing changes.
            auto refusedWith = [&](const std::string& id, double expected, double current, const CalibrationIdentity& cur,
                                   CalibrationApplyOutcome outcome) {
                const auto r = store.apply(id, expected, current, cur, t0 + 10);
                return r.outcome == outcome && store.revision() == rev && !store.applied() && !r.newFactor;
            };
            MIB_EXPECT(refusedWith("cal-1", 0.5, 0.4886, identityA(), CalibrationApplyOutcome::StaleFactor),
                       "a stale expected factor is refused");
            MIB_EXPECT(refusedWith("cal-few", 0.4886, 0.4886, identityA(), CalibrationApplyOutcome::Insufficient),
                       "an insufficient record can never be applied");
            MIB_EXPECT(refusedWith("cal-none", 0.4886, 0.4886, identityA(), CalibrationApplyOutcome::NotFound),
                       "an unknown record");
            auto other = identityA();
            other.camera = "egrabber:Coaxlink 0";
            MIB_EXPECT(refusedWith("cal-1", 0.4886, 0.4886, other, CalibrationApplyOutcome::IdentityMismatch),
                       "a record of another setup is not applied here");
            MIB_EXPECT(refusedWith("cal-1", 0.4886, 0.4886, CalibrationIdentity{}, CalibrationApplyOutcome::IdentityMismatch),
                       "an unknown current identity refuses");
            MIB_EXPECT(refusedWith("", 0.4886, 0.4886, identityA(), CalibrationApplyOutcome::InvalidRequest), "empty id");
            MIB_EXPECT(refusedWith("cal-1", std::nan(""), 0.4886, identityA(), CalibrationApplyOutcome::InvalidRequest),
                       "a non-finite expected factor");
            store.setFailWritesForTesting(true);
            MIB_EXPECT(refusedWith("cal-1", 0.4886, 0.4886, identityA(), CalibrationApplyOutcome::StoreFailed),
                       "a failed write applies nothing");
            store.setFailWritesForTesting(false);

            // Apply: previous vs new.
            const auto a1 = store.apply("cal-1", 0.4886, 0.4886, identityA(), t0 + 20);
            MIB_REQUIRE(a1.ok(), "apply: " + a1.message);
            MIB_EXPECT(a1.previousFactor == 0.4886 && a1.newFactor == proposal && a1.supersededRecordId.empty(),
                       "the reply carries previous vs new");
            MIB_EXPECT(store.applied() && store.applied()->recordId == "cal-1" && store.applied()->version == 2,
                       "applied as a new version");
            MIB_EXPECT(store.history("cal-1").size() == 2 && store.history("cal-1")[0].status == CalibrationStatus::Proposed,
                       "append-only: the proposed version is kept");
            MIB_EXPECT(store.apply("cal-1", proposal, proposal, identityA(), t0 + 21).outcome ==
                           CalibrationApplyOutcome::NotApplicable,
                       "an applied record in effect is not applied twice");
            const auto reapply = store.apply("cal-1", 0.6, 0.6, identityA(), t0 + 22);
            MIB_EXPECT(reapply.ok() && reapply.newFactor == proposal && reapply.previousFactor == 0.6,
                       "an applied record whose factor was changed since can be re-applied explicitly");

            // A second proposal supersedes the first.
            MIB_REQUIRE(store.addDerived(derivedRecord("cal-2", beads(300, 14.0, 0.03), 0.4886, identityA(), t0 + 30), &why),
                        "add cal-2");
            const auto a2 = store.apply("cal-2", proposal, proposal, identityA(), t0 + 40);
            MIB_EXPECT(a2.ok() && a2.supersededRecordId == "cal-1" && store.applied()->recordId == "cal-2" &&
                           store.latest("cal-1")->status == CalibrationStatus::Superseded &&
                           store.latest("cal-1")->supersededBy == "cal-2",
                       "the previous applied record is superseded");
            MIB_EXPECT(store.apply("cal-1", *a2.newFactor, *a2.newFactor, identityA(), t0 + 41).outcome ==
                           CalibrationApplyOutcome::NotApplicable,
                       "a superseded record is not applied again");

            // Hard invalidation on an identity change; an unknown identity never invalidates.
            MIB_EXPECT(!store.invalidateOnIdentityChange(CalibrationIdentity{}, t0 + 50) && store.applied(),
                       "unknown identity: nothing invalidated");
            auto roiChanged = identityA();
            roiChanged.roi = "0,0,64,64";
            std::string note;
            MIB_EXPECT(store.invalidateOnIdentityChange(roiChanged, t0 + 60, &note) && !store.applied() &&
                           store.latest("cal-2")->status == CalibrationStatus::Invalidated &&
                           note.find("ROI 0,0,96,96 -> 0,0,64,64") != std::string::npos,
                       "an ROI change invalidates the applied record: " + note);
            MIB_EXPECT(store.apply("cal-2", *a2.newFactor, *a2.newFactor, identityA(), t0 + 70).outcome ==
                           CalibrationApplyOutcome::NotApplicable,
                       "an invalidated record is not applied again");
        }
        {
            // Persistence: a reload sees every version in order.
            CalibrationStore reloaded(file);
            MIB_EXPECT(reloaded.error().empty(), "reload: " + reloaded.error());
            MIB_EXPECT(reloaded.latestRecords().size() == 3 && reloaded.history("cal-1").size() == 4 &&
                           reloaded.latest("cal-2")->status == CalibrationStatus::Invalidated && !reloaded.applied(),
                       "versions survive a reload");
            MIB_EXPECT(reloaded.latestRecords().front().recordId == "cal-2", "newest record first");
        }
        {
            // An unreadable store is left untouched and refuses appends.
            const auto broken = td.path() / "broken.json";
            {
                std::ofstream out(broken, std::ios::binary);
                out << "{not json";
            }
            CalibrationStore store(broken);
            MIB_EXPECT(!store.error().empty(), "unreadable store reports why");
            std::string why;
            MIB_EXPECT(!store.addDerived(derivedRecord("cal-x", beads(300, 15.0, 0.03), 0.4886, identityA(), t0), &why),
                       "an unreadable store adds nothing");
            std::ifstream in(broken, std::ios::binary);
            std::string text((std::istreambuf_iterator<char>(in)), {});
            MIB_EXPECT(text == "{not json", "the file is left untouched");
        }
    }
    return mib::test::exitCode();
}
