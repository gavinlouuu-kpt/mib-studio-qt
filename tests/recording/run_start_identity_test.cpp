// run_start_identity_test
//
// What a client declares when it starts a run (#712 PR 2, ADR 0015 items 1, 4, 5, 7; bridge ABI 39):
// the strict startRequest parser, the identity members of the run snapshot v3 and the
// /experiment_annotation block (coverage matrix: Save data -> round-trip + legacy read + fault
// injection).
//  Strict request:
//  - a full request parses; every bound accepts its limit and refuses one past it (never truncated);
//  - unknown and duplicate keys, wrong types, empty strings, control characters, invalid UTF-8,
//    out-of-range numbers and identity without an experiment are refused with the key named.
//  Round trip:
//  - identity members written by runProvenanceV3Members parse back; null and "" read as unknown;
//  - the annotation document round-trips, free key/values sorted by key; a file holds it once.
//  Legacy read:
//  - v2 and PR-1 v3 snapshots read the identity as unknown; a file without /experiment_annotation
//    has no annotation (schema 0).
//  Fault injection:
//  - the annotation write is refused with no file, on a read-only file, when the block exists, and
//    under the injected fault (nothing is left behind); a flush that fails after the attribute writes
//    fails the write.

#include "backend/recording/Hdf5Service.h"
#include "backend/recording/RunProvenanceRecord.h"
#include "backend/recording/RunStartIdentity.h"

#include "support/assert.h"
#include "support/tempdir.h"

#include <hdf5.h>

#include <cstdio>
#include <limits>
#include <string>

using backend::services::Hdf5Service;
namespace rec = backend::recording;

namespace {

const char* kFull = R"({"experiment_id":"3F2A9C1E-7B4D-4E8A-9C2F-1A2B3C4D5E6F","run_index":3,"sample_ref":"S3",
  "operator":{"id":"op-12","name":"Alice"},"calibration_record_id":"cal-2026-10-19",
  "annotation":{"sample":"donor A, 10 uM","condition":"drug X","experimental_unit":"donor A",
  "concentration":{"value":10,"unit":"uM"},"channel":"20 um","medium":"PBS + 0.5% MC",
  "temperature_c":21.5,"notes":"line 1\nline 2\ttab","free":{"passage":"4","batch":"B-7"}}})";

bool refused(const std::string& text, const std::string& key)
{
    rec::StartRequestIdentity out;
    out.identity.sampleRef = "stale";
    std::string why;
    const bool ok = rec::parseStartRequestJson(text, out, &why);
    const bool named = why.find(key) != std::string::npos;
    if (ok || !named) std::fprintf(stderr, "  request %s -> ok=%d why=%s\n", text.c_str(), ok ? 1 : 0, why.c_str());
    return !ok && named && out.identity.empty() && !out.annotation;
}

bool accepted(const std::string& text)
{
    rec::StartRequestIdentity out;
    std::string why;
    const bool ok = rec::parseStartRequestJson(text, out, &why);
    if (!ok) std::fprintf(stderr, "  request %s refused: %s\n", text.substr(0, 120).c_str(), why.c_str());
    return ok;
}

std::string quoted(std::size_t n, char c = 'x') { return "\"" + std::string(n, c) + "\""; }

const char* kExp = "\"experiment_id\":\"3f2a9c1e-7b4d-4e8a-9c2f-1a2b3c4d5e6f\"";

} // namespace

int main()
{
    mib::test::TempDir td("mib_run_start_identity");

    // ---- a full request ----------------------------------------------------------------
    rec::StartRequestIdentity full;
    {
        std::string why;
        MIB_REQUIRE(rec::parseStartRequestJson(kFull, full, &why), "full request parses: " + why);
        const auto& id = full.identity;
        MIB_EXPECT(id.experimentId == std::optional<std::string>("3f2a9c1e-7b4d-4e8a-9c2f-1a2b3c4d5e6f"),
                   "experiment id stored lowercase");
        MIB_EXPECT(id.runIndex == std::optional<uint32_t>(3) && id.sampleRef == std::optional<std::string>("S3") &&
                       id.selectedOperator && id.selectedOperator->id == std::optional<std::string>("op-12") &&
                       id.selectedOperator->name == std::optional<std::string>("Alice") &&
                       id.calibrationRecordId == std::optional<std::string>("cal-2026-10-19"),
                   "identity members");
        MIB_REQUIRE(full.annotation.has_value(), "annotation parsed");
        const auto& a = *full.annotation;
        MIB_EXPECT(a.condition == std::optional<std::string>("drug X") &&
                       a.experimentalUnit == std::optional<std::string>("donor A") && a.concentration &&
                       a.concentration->value == 10.0 && a.concentration->unit == "uM" &&
                       a.temperatureC == std::optional<double>(21.5) &&
                       a.medium == std::optional<std::string>("PBS + 0.5% MC") &&
                       a.notes == std::optional<std::string>("line 1\nline 2\ttab"),
                   "annotation fixed members");
        MIB_EXPECT(a.free.size() == 2 && a.free[0].first == "batch" && a.free[0].second == "B-7" &&
                       a.free[1].first == "passage",
                   "free key/values, sorted by key");
        rec::StartRequestIdentity none;
        MIB_EXPECT(rec::parseStartRequestJson("", none, &why) && none.identity.empty() && !none.annotation,
                   "an empty request declares nothing");
        MIB_EXPECT(rec::parseStartRequestJson("null", none, &why) && none.identity.empty() && !none.annotation,
                   "a null request declares nothing");
        MIB_EXPECT(accepted("{\"experiment_id\":null,\"operator\":null,\"annotation\":null}"),
                   "null members are 'not declared'");
        MIB_EXPECT(accepted("{\"annotation\":{}}"), "an empty annotation object is a (blank) annotation");
    }

    // ---- refusals: shape and types --------------------------------------------------------
    {
        MIB_EXPECT(refused("{\"operator_name\":\"A\"}", "operator_name"), "unknown key");
        MIB_EXPECT(refused("{\"annotation\":{\"donor\":\"A\"}}", "donor"), "unknown annotation key");
        MIB_EXPECT(refused("{\"operator\":{\"id\":\"a\",\"role\":\"x\"}}", "role"), "unknown operator key");
        MIB_EXPECT(refused("{\"annotation\":{\"concentration\":{\"value\":1,\"unit\":\"uM\",\"x\":1}}}", "concentration"),
                   "unknown concentration key");
        MIB_EXPECT(refused(std::string("{") + kExp + ",\"sample_ref\":\"a\",\"sample_ref\":\"b\"}", "duplicate key"),
                   "duplicate key");
        MIB_EXPECT(refused("{\"annotation\":{\"free\":{\"k\":\"a\",\"k\":\"b\"}}}", "duplicate key"),
                   "duplicate free key");
        MIB_EXPECT(refused("[1]", "startRequest"), "not an object");
        MIB_EXPECT(refused("{oops", "not valid JSON"), "not JSON");
        MIB_EXPECT(refused("{\"annotation\":{\"free\":{\"k\":7}}}", "annotation.free"), "free values are strings only");
        MIB_EXPECT(refused("{\"annotation\":{\"free\":[\"k\"]}}", "annotation.free"), "free is an object");
        MIB_EXPECT(refused("{\"annotation\":{\"temperature_c\":\"21\"}}", "temperature_c"), "temperature is a number");
        MIB_EXPECT(refused(std::string("{") + kExp + ",\"run_index\":1.5}", "run_index"), "run_index is whole");
        MIB_EXPECT(refused(std::string("{") + kExp + ",\"run_index\":-1}", "run_index"), "run_index >= 0");
        MIB_EXPECT(refused(std::string("{") + kExp + ",\"run_index\":4294967296}", "run_index"), "run_index fits u32");
        MIB_EXPECT(accepted(std::string("{") + kExp + ",\"run_index\":4294967295}"), "run_index max");
        MIB_EXPECT(refused("{\"operator\":\"Alice\"}", "operator"), "operator is an object");
        MIB_EXPECT(refused("{\"operator\":{}}", "operator"), "an operator needs an id or a name");
        MIB_EXPECT(refused("{\"annotation\":{\"concentration\":{\"value\":1}}}", "concentration"),
                   "concentration needs its unit");
        MIB_EXPECT(refused("{\"annotation\":{\"concentration\":{\"value\":-1,\"unit\":\"uM\"}}}", "concentration.value"),
                   "concentration >= 0");
    }

    // ---- refusals: values ----------------------------------------------------------------------
    {
        MIB_EXPECT(refused("{\"experiment_id\":\"not-a-uuid\"}", "experiment_id"), "experiment id is a UUID");
        MIB_EXPECT(refused("{\"experiment_id\":\"3f2a9c1e7b4d4e8a9c2f1a2b3c4d5e6f\"}", "experiment_id"),
                   "UUID in canonical 8-4-4-4-12 form");
        MIB_EXPECT(refused("{\"run_index\":1}", "run_index"), "run_index needs an experiment");
        MIB_EXPECT(refused("{\"sample_ref\":\"S1\"}", "sample_ref"), "sample_ref needs an experiment");
        MIB_EXPECT(refused("{\"experiment_id\":\"\"}", "experiment_id"), "\"\" is not an id");
        MIB_EXPECT(refused("{\"calibration_record_id\":\"\"}", "calibration_record_id"), "\"\" is refused (use null)");
        MIB_EXPECT(refused("{\"annotation\":{\"condition\":\"\"}}", "annotation.condition"), "\"\" annotation text");
        MIB_EXPECT(refused("{\"annotation\":{\"free\":{\"\":\"v\"}}}", "annotation.free"), "empty free key");
        MIB_EXPECT(refused("{\"annotation\":{\"free\":{\"k\":\"\"}}}", "annotation.free"), "empty free value");
        MIB_EXPECT(refused("{\"operator\":{\"name\":\"A\\u0007\"}}", "operator.name"), "control character");
        MIB_EXPECT(refused("{\"annotation\":{\"condition\":\"a\\nb\"}}", "annotation.condition"),
                   "newline only in notes");
        MIB_EXPECT(accepted("{\"annotation\":{\"notes\":\"a\\nb\\r\\n\\tc\"}}"), "notes may hold tab, CR, LF");
        MIB_EXPECT(refused("{\"annotation\":{\"temperature_c\":150.5}}", "temperature_c"), "temperature above the bound");
        MIB_EXPECT(refused("{\"annotation\":{\"temperature_c\":-50.5}}", "temperature_c"), "temperature below the bound");
        MIB_EXPECT(accepted("{\"annotation\":{\"temperature_c\":-50}}") &&
                       accepted("{\"annotation\":{\"temperature_c\":150}}"),
                   "temperature bounds inclusive");
        // Invalid UTF-8 cannot reach a parsed document (the JSON parser refuses it) ...
        MIB_EXPECT(refused("{\"sample_ref\":\"\xC3\x28\"}", "not valid JSON"), "invalid UTF-8 in JSON");
        // ... and a typed request built in C++ is checked too.
        rec::StartRequestIdentity typed;
        typed.identity.calibrationRecordId = std::string("\xF0\x9F\x98", 3); // truncated 4-byte sequence
        std::string why;
        MIB_EXPECT(!rec::validateStartRequest(typed, &why) && why.find("UTF-8") != std::string::npos,
                   "typed invalid UTF-8 refused: " + why);
        typed.identity.calibrationRecordId = std::string("\xF0\x9F\x98\x80"); // a valid emoji
        MIB_EXPECT(rec::validateStartRequest(typed, &why), "valid 4-byte UTF-8 accepted");
        typed.identity.experimentId = "3F2A9C1E-7B4D-4E8A-9C2F-1A2B3C4D5E6F";
        MIB_EXPECT(!rec::validateStartRequest(typed, &why), "typed requests carry the lowercase form");
    }

    // ---- bounds: the limit is accepted, one past it refused (never truncated) ---------------
    {
        const auto atAndPast = [](const std::string& prefix, std::size_t limit, const std::string& suffix,
                                  const std::string& key) {
            return accepted(prefix + quoted(limit) + suffix) && refused(prefix + quoted(limit + 1) + suffix, key);
        };
        const std::string exp = std::string("{") + kExp + ",";
        MIB_EXPECT(atAndPast(exp + "\"sample_ref\":", rec::kStartIdMaxBytes, "}", "sample_ref"), "sample_ref bound");
        MIB_EXPECT(atAndPast("{\"calibration_record_id\":", rec::kStartIdMaxBytes, "}", "calibration_record_id"),
                   "calibration id bound");
        MIB_EXPECT(atAndPast("{\"operator\":{\"name\":", rec::kStartIdMaxBytes, "}}", "operator.name"),
                   "operator name bound");
        MIB_EXPECT(atAndPast("{\"annotation\":{\"medium\":", rec::kAnnotationTextMaxBytes, "}}", "annotation.medium"),
                   "annotation text bound");
        MIB_EXPECT(atAndPast("{\"annotation\":{\"notes\":", rec::kAnnotationNotesMaxBytes, "}}", "annotation.notes"),
                   "notes bound");
        MIB_EXPECT(atAndPast("{\"annotation\":{\"concentration\":{\"value\":1,\"unit\":", rec::kAnnotationUnitMaxBytes,
                             "}}}", "annotation.concentration.unit"),
                   "unit bound");
        MIB_EXPECT(atAndPast("{\"annotation\":{\"free\":{", rec::kAnnotationFreeKeyMaxBytes, ":\"v\"}}}",
                             "annotation.free"),
                   "free key bound");
        MIB_EXPECT(atAndPast("{\"annotation\":{\"free\":{\"k\":", rec::kAnnotationFreeValueMaxBytes, "}}}",
                             "annotation.free"),
                   "free value bound");
        auto freeEntries = [](std::size_t n) {
            std::string doc = "{\"annotation\":{\"free\":{";
            for (std::size_t i = 0; i < n; ++i) doc += (i ? ",\"k" : "\"k") + std::to_string(i) + "\":\"v\"";
            return doc + "}}}";
        };
        MIB_EXPECT(accepted(freeEntries(rec::kAnnotationFreeMaxEntries)) &&
                       refused(freeEntries(rec::kAnnotationFreeMaxEntries + 1), "annotation.free"),
                   "free entry count bound");
        const std::string big = "{\"annotation\":{\"notes\":" + quoted(rec::kStartRequestMaxBytes) + "}}";
        MIB_EXPECT(refused(big, "startRequest"), "the whole document is bounded");
    }

    // ---- identity in the snapshot: round trip, unknown, "" and legacy ----------------------
    {
        rec::RunProvenanceV3 v;
        v.identity = full.identity;
        const std::string doc = "{\"schema_version\":3," + rec::runProvenanceV3Members(v) + "}";
        MIB_EXPECT(doc.find("\"assurance\":\"selected\"") != std::string::npos, "the operator is recorded as selected");
        rec::RecordedRunProvenance back;
        MIB_REQUIRE(rec::parseRunProvenance(doc, back), "parse identity");
        const auto& id = back.v3.identity;
        MIB_EXPECT(id.experimentId == full.identity.experimentId && id.runIndex == full.identity.runIndex &&
                       id.sampleRef == full.identity.sampleRef && id.selectedOperator &&
                       id.selectedOperator->id == full.identity.selectedOperator->id &&
                       id.selectedOperator->name == full.identity.selectedOperator->name &&
                       id.calibrationRecordId == full.identity.calibrationRecordId,
                   "identity round trip");

        rec::RunProvenanceV3 blank;
        const std::string members = rec::runProvenanceV3Members(blank);
        MIB_EXPECT(members.find("\"experiment_id\":null") != std::string::npos &&
                       members.find("\"run_index\":null") != std::string::npos &&
                       members.find("\"operator\":null") != std::string::npos &&
                       members.find("\"calibration_record_id\":null") != std::string::npos,
                   "undeclared identity is written as null: " + members);
        MIB_REQUIRE(rec::parseRunProvenance("{\"schema_version\":3," + members + "}", back), "parse blank");
        MIB_EXPECT(back.v3.identity.empty(), "null identity reads as unknown");

        MIB_REQUIRE(rec::parseRunProvenance("{\"schema_version\":3,\"experiment_id\":\"\",\"sample_ref\":\"\","
                                            "\"operator\":{\"id\":\"\",\"name\":\"\"},\"run_index\":-2}",
                                            back),
                    "parse foreign empties");
        MIB_EXPECT(back.v3.identity.empty(), "\"\", an empty operator and a negative index read as unknown");

        MIB_REQUIRE(rec::parseRunProvenance("{\"schema_version\":3,\"run_id\":\"r\",\"pl_run_id\":null}", back),
                    "a PR-1 v3 snapshot");
        MIB_EXPECT(back.v3.identity.empty() && back.v3.runId, "v3 without identity members: identity unknown");
        MIB_REQUIRE(rec::parseRunProvenance("{\"schema_version\":2,\"profile_id\":\"p\"}", back), "v2");
        MIB_EXPECT(back.v3.identity.empty(), "v2: identity unknown");
    }

    // ---- annotation codec ------------------------------------------------------------------------
    {
        const std::string json = rec::experimentAnnotationToJson(*full.annotation);
        MIB_EXPECT(json.find("\"schema_version\":1") != std::string::npos, "annotation document versioned");
        const auto back = rec::parseExperimentAnnotation(json);
        MIB_REQUIRE(back.has_value(), "annotation parses back");
        MIB_EXPECT(back->sample == full.annotation->sample && back->condition == full.annotation->condition &&
                       back->experimentalUnit == full.annotation->experimentalUnit && back->concentration &&
                       back->concentration->value == 10.0 && back->concentration->unit == "uM" &&
                       back->channel == full.annotation->channel && back->medium == full.annotation->medium &&
                       back->temperatureC == full.annotation->temperatureC && back->notes == full.annotation->notes &&
                       back->free == full.annotation->free,
                   "annotation round trip (free key/values sorted by key)");

        rec::ExperimentAnnotation blank;
        const std::string blankJson = rec::experimentAnnotationToJson(blank);
        MIB_EXPECT(blankJson.find("\"condition\":null") != std::string::npos &&
                       blankJson.find("\"concentration\":null") != std::string::npos &&
                       blankJson.find("\"temperature_c\":null") != std::string::npos &&
                       blankJson.find("\"free\":{}") != std::string::npos,
                   "undeclared annotation members are null: " + blankJson);
        const auto tolerant = rec::parseExperimentAnnotation(
            "{\"schema_version\":1,\"condition\":\"\",\"medium\":5,\"concentration\":{\"value\":1},"
            "\"free\":{\"a\":\"1\",\"b\":2,\"c\":\"\",\"\":\"x\"}}");
        MIB_EXPECT(tolerant && !tolerant->condition && !tolerant->medium && !tolerant->concentration &&
                       tolerant->free.size() == 1 && tolerant->free[0].first == "a",
                   "reader: \"\", wrong types and a unit-less concentration are unknown; bad free entries skipped");
        MIB_EXPECT(!rec::parseExperimentAnnotation("[1]") && !rec::parseExperimentAnnotation("{x"),
                   "a non-object annotation is rejected");
    }

    // ---- the /experiment_annotation block in a file ---------------------------------------------
    const std::string path = (td / "annotated.h5").string();
    {
        Hdf5Service w;
        MIB_REQUIRE(w.openFile(path) && w.initializeDatasets(), "create file");
        rec::RunProvenanceV3 v;
        v.identity = full.identity;
        MIB_REQUIRE(w.writeRunSnapshotJson("{\"schema_version\":3," + rec::runProvenanceV3Members(v) + "}", "{}"),
                    "snapshot");
        const std::string doc = rec::experimentAnnotationToJson(*full.annotation);
        MIB_REQUIRE(w.writeExperimentAnnotationJson(doc), "annotation written");
        MIB_EXPECT(!w.writeExperimentAnnotationJson(rec::experimentAnnotationToJson(rec::ExperimentAnnotation{})),
                   "the block is written once: a second write is refused");
        w.closeFile();

        Hdf5Service r;
        MIB_REQUIRE(r.loadFile(path), "reload");
        std::string raw;
        uint64_t version = 0;
        MIB_EXPECT(r.readExperimentAnnotationJson(raw, &version) && raw == doc && version == 1,
                   "annotation and its schema version read back unchanged");
        rec::RecordedRunProvenance prov;
        MIB_REQUIRE(rec::readRecordedRunProvenance(r, prov), "typed read");
        MIB_EXPECT(prov.annotation && prov.annotationSchemaVersion == 1 && prov.annotation->free == full.annotation->free &&
                       prov.v3.identity.sampleRef == full.identity.sampleRef,
                   "typed read: identity and annotation");
        MIB_EXPECT(!r.writeExperimentAnnotationJson(doc), "a read-only file refuses the annotation");
        r.closeFile();
    }
    {
        Hdf5Service none;
        std::string raw = "stale";
        uint64_t version = 9;
        MIB_EXPECT(!none.writeExperimentAnnotationJson("{}") && !none.readExperimentAnnotationJson(raw, &version) &&
                       raw.empty() && version == 0,
                   "no file: annotation refused, read fails and clears");

        const std::string plain = (td / "no_annotation.h5").string();
        Hdf5Service w;
        MIB_REQUIRE(w.openFile(plain) && w.initializeDatasets(), "create file without annotation");
        MIB_REQUIRE(w.writeRunSnapshotJson("{\"schema_version\":3}", "{}"), "snapshot only");
        w.setExperimentAnnotationWriteFaultForTesting(true);
        MIB_EXPECT(!w.writeExperimentAnnotationJson("{\"schema_version\":1}"), "injected fault refuses the write");
        w.setExperimentAnnotationWriteFaultForTesting(false);

        // The attributes are written but the flush fails: the write must report failure.
        const std::string flushPath = (td / "flush_fault.h5").string();
        Hdf5Service f;
        MIB_REQUIRE(f.openFile(flushPath) && f.initializeDatasets(), "create flush-fault file");
        f.setExperimentAnnotationFlushFaultForTesting(true);
        MIB_EXPECT(!f.writeExperimentAnnotationJson("{\"schema_version\":1}"),
                   "a failed flush after the attribute writes fails the annotation write");
        f.setExperimentAnnotationFlushFaultForTesting(false);
        f.closeFile();
        w.closeFile();
        Hdf5Service r;
        MIB_REQUIRE(r.loadFile(plain), "reload");
        rec::RecordedRunProvenance prov;
        MIB_EXPECT(rec::readRecordedRunProvenance(r, prov) && !prov.annotation && prov.annotationSchemaVersion == 0,
                   "the run reads as having no annotation");
        r.closeFile();
        const hid_t raw5 = H5Fopen(plain.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
        MIB_REQUIRE(raw5 >= 0, "raw open");
        MIB_EXPECT(H5Lexists(raw5, "/experiment_annotation", H5P_DEFAULT) <= 0, "no block left behind by the fault");
        H5Fclose(raw5);
    }

    return mib::test::exitCode();
}
