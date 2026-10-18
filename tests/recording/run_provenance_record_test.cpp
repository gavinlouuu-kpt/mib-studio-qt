// run_provenance_record_test
//
// Run snapshot v3 (#712, ADR 0015 item 5): the codec and the typed reader of the members v3 adds
// to /run_provenance (coverage matrix: Save data -> round-trip + fault-injection).
//  Round trip:
//  - every member written by runProvenanceV3Members parses back to the same value, in memory and
//    through an HDF5 file (run_snapshot_json + run_stop_json), with run_snapshot_schema_version 3;
//  - unknown facts are written as null (NaN too) and read back as nullopt, never as 0.
//  Legacy read:
//  - v1 (no schema_version, no method block) and v2 documents read with every v3 member nullopt;
//  - a file without /run_provenance has no snapshot (schema 0) and no stop record.
//  Fault injection:
//  - writes are refused with no file open and on a read-only file (nothing is left behind);
//  - garbage JSON in either attribute is read verbatim and then rejected (no snapshot, no stop);
//  - a member of the wrong type reads as absent; a non-string attribute reads as absent;
//  - a snapshot the file cannot hold fails the write and leaves no version attribute behind.

#include "backend/recording/Hdf5Service.h"
#include "backend/recording/RunProvenanceRecord.h"

#include "support/assert.h"
#include "support/tempdir.h"

#include <hdf5.h>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

using backend::services::Hdf5Service;
namespace rec = backend::recording;

namespace {

rec::RunProvenanceV3 fullRecord()
{
    rec::RunProvenanceV3 v;
    v.runId = "3f2a9c1e-7b4d-4e8a-9c2f-1a2b3c4d5e6f";
    rec::EModulusLutRecord lut;
    lut.loaded = true;
    lut.lutId = "scaled_isoelastic_data_LUT_6.16-4.24.txt";
    lut.revision = "2026-09-01";
    lut.source = "r2-public-catalog";
    lut.sha256 = std::string(64, 'a');
    lut.catalogSha256 = std::string(64, 'A'); // case-insensitive match
    lut.checksumStatus = "verified";
    v.eModulusLut = lut;

    rec::PumpRecord sample;
    sample.slot = "sample";
    sample.connected = true;
    sample.model = "tushui_peristaltic";
    sample.port = "/dev/ttyPS1";
    sample.modbusAddress = 3;
    sample.flowRate = 10.0;
    sample.flowRateUnitCode = 100;
    sample.flowRateUnit = "uL/min";
    sample.direction = "infuse";
    sample.microlitersPerRev = 25.0;
    rec::PumpReadbackRecord rb;
    rb.ageUs = 125000;
    rb.lastPollOk = false;
    rb.runStatus = "forward";
    rb.speedRpm = 0.4;
    sample.readback = rb;
    rec::PumpRecord sheath; // connected, never polled; unknown unit code
    sheath.slot = "sheath";
    sheath.connected = true;
    sheath.model = "dlsp_syringe";
    sheath.modbusAddress = 1;
    sheath.flowRate = 50.0;
    sheath.flowRateUnitCode = 7;
    sheath.direction = "withdraw";
    rec::PumpRecord busy; // not sampled at Start
    busy.slot = "spare";
    v.pumps = std::vector<rec::PumpRecord>{sample, sheath, busy};

    rec::LedRecord led;
    led.control = 1;
    led.delayUs = 7.0;
    led.widthUs = 60.0;
    led.clockKhz = 100000;
    led.stoppedRunAtStart = true;
    v.led = led;

    rec::StageRecord stage;
    stage.connected = true;
    stage.model = "ZC300-1A";
    stage.serial = "26017";
    stage.firmware = "1.2";
    stage.zeroSet = true;
    rec::StageReadbackRecord srb;
    srb.ageUs = 40;
    srb.state = "idle";
    srb.positionUm = -12.25;
    srb.limitNegative = true;
    stage.readback = srb;
    v.stage = stage;

    rec::PiezoRecord piezo;
    piezo.connected = true;
    piezo.voltageV = 61.5;
    piezo.autofocusEnabled = false;
    v.piezo = piezo;
    v.plRunId = 1760850000123ull;
    return v;
}

std::string snapshotDoc(const rec::RunProvenanceV3& v)
{
    return "{\"schema_version\":3,\"profile_id\":\"p\"," + rec::runProvenanceV3Members(v) + "}";
}

void expectEqual(const rec::RunProvenanceV3& a, const rec::RunProvenanceV3& b, const std::string& where)
{
    MIB_EXPECT(a.runId == b.runId, where + ": run id");
    MIB_REQUIRE(a.eModulusLut && b.eModulusLut, where + ": LUT present");
    MIB_EXPECT(a.eModulusLut->loaded == b.eModulusLut->loaded && a.eModulusLut->lutId == b.eModulusLut->lutId &&
                   a.eModulusLut->revision == b.eModulusLut->revision && a.eModulusLut->source == b.eModulusLut->source &&
                   a.eModulusLut->sha256 == b.eModulusLut->sha256 &&
                   a.eModulusLut->catalogSha256 == b.eModulusLut->catalogSha256 &&
                   a.eModulusLut->checksumStatus == b.eModulusLut->checksumStatus,
               where + ": LUT members");
    MIB_REQUIRE(a.pumps && b.pumps && a.pumps->size() == b.pumps->size(), where + ": pumps");
    for (size_t i = 0; i < a.pumps->size(); ++i) {
        const auto& p = (*a.pumps)[i];
        const auto& q = (*b.pumps)[i];
        MIB_EXPECT(p.slot == q.slot && p.connected == q.connected && p.model == q.model && p.port == q.port &&
                       p.modbusAddress == q.modbusAddress && p.flowRate == q.flowRate &&
                       p.flowRateUnitCode == q.flowRateUnitCode && p.flowRateUnit == q.flowRateUnit &&
                       p.direction == q.direction && p.microlitersPerRev == q.microlitersPerRev,
                   where + ": pump " + p.slot + " setpoint");
        MIB_EXPECT(p.readback.has_value() == q.readback.has_value(), where + ": pump " + p.slot + " readback presence");
        if (p.readback && q.readback) {
            MIB_EXPECT(p.readback->ageUs == q.readback->ageUs && p.readback->lastPollOk == q.readback->lastPollOk &&
                           p.readback->runStatus == q.readback->runStatus && p.readback->speedRpm == q.readback->speedRpm &&
                           p.readback->realtimeFlowRaw == q.readback->realtimeFlowRaw &&
                           p.readback->accumulatedVolumeRaw == q.readback->accumulatedVolumeRaw &&
                           p.readback->stalled == q.readback->stalled,
                       where + ": pump " + p.slot + " readback");
        }
    }
    MIB_REQUIRE(a.led && b.led, where + ": LED");
    MIB_EXPECT(a.led->source == b.led->source && a.led->control == b.led->control && a.led->delayUs == b.led->delayUs &&
                   a.led->widthUs == b.led->widthUs && a.led->clockKhz == b.led->clockKhz &&
                   a.led->stoppedRunAtStart == b.led->stoppedRunAtStart && a.led->error == b.led->error,
               where + ": LED members");
    MIB_REQUIRE(a.stage && b.stage && a.stage->readback && b.stage->readback, where + ": stage");
    MIB_EXPECT(a.stage->connected == b.stage->connected && a.stage->model == b.stage->model &&
                   a.stage->serial == b.stage->serial && a.stage->firmware == b.stage->firmware &&
                   a.stage->zeroSet == b.stage->zeroSet && a.stage->readback->ageUs == b.stage->readback->ageUs &&
                   a.stage->readback->state == b.stage->readback->state &&
                   a.stage->readback->positionUm == b.stage->readback->positionUm &&
                   a.stage->readback->limitNegative == b.stage->readback->limitNegative &&
                   a.stage->readback->limitPositive == b.stage->readback->limitPositive,
               where + ": stage members");
    MIB_REQUIRE(a.piezo && b.piezo, where + ": piezo");
    MIB_EXPECT(a.piezo->connected == b.piezo->connected && a.piezo->voltageV == b.piezo->voltageV &&
                   a.piezo->autofocusEnabled == b.piezo->autofocusEnabled,
               where + ": piezo members");
    MIB_EXPECT(a.plRunId == b.plRunId, where + ": PL run id");
}

bool allUnknown(const rec::RunProvenanceV3& v)
{
    return !v.runId && !v.eModulusLut && !v.pumps && !v.led && !v.stage && !v.piezo && !v.plRunId;
}

// Raw HDF5: write a value of another type under an attribute name (a foreign writer).
bool writeForeignU64(const std::string& path, const char* name)
{
    hid_t file = H5Fopen(path.c_str(), H5F_ACC_RDWR, H5P_DEFAULT);
    if (file < 0) return false;
    hid_t group = H5Gopen2(file, "/run_provenance", H5P_DEFAULT);
    bool ok = group >= 0;
    if (ok) {
        if (H5Aexists(group, name) > 0) H5Adelete(group, name);
        hid_t space = H5Screate(H5S_SCALAR);
        hid_t attr = H5Acreate2(group, name, H5T_NATIVE_UINT64, space, H5P_DEFAULT, H5P_DEFAULT);
        const uint64_t v = 7;
        ok = attr >= 0 && H5Awrite(attr, H5T_NATIVE_UINT64, &v) >= 0;
        if (attr >= 0) H5Aclose(attr);
        H5Sclose(space);
        H5Gclose(group);
    }
    H5Fclose(file);
    return ok;
}

uint64_t schemaAttribute(const std::string& path, bool* present)
{
    *present = false;
    uint64_t v = 0;
    hid_t file = H5Fopen(path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
    if (file < 0) return 0;
    if (H5Lexists(file, "/run_provenance", H5P_DEFAULT) > 0) {
        hid_t group = H5Gopen2(file, "/run_provenance", H5P_DEFAULT);
        if (H5Aexists(group, "run_snapshot_schema_version") > 0) {
            hid_t attr = H5Aopen(group, "run_snapshot_schema_version", H5P_DEFAULT);
            *present = H5Aread(attr, H5T_NATIVE_UINT64, &v) >= 0;
            H5Aclose(attr);
        }
        H5Gclose(group);
    }
    H5Fclose(file);
    return v;
}

} // namespace

int main()
{
    mib::test::TempDir td("mib_run_provenance");

    // ---- codec round trip ---------------------------------------------------------
    const auto full = fullRecord();
    {
        rec::RecordedRunProvenance back;
        std::string why;
        MIB_REQUIRE(rec::parseRunProvenance(snapshotDoc(full), back, &why), "parse: " + why);
        MIB_EXPECT(back.schemaVersion == 3, "schema version from the document");
        expectEqual(full, back.v3, "codec");
        MIB_EXPECT(back.v3.eModulusLut->catalogMatchesLoaded() == std::optional<bool>(true),
                   "catalog hash compared case-insensitively");
        MIB_EXPECT(back.v3.led->enabled() == std::optional<bool>(true), "LED enabled from S[0]");
        const auto& sheath = (*back.v3.pumps)[1];
        MIB_EXPECT(sheath.flowRateUnitCode == 7 && !sheath.flowRateUnit && !sheath.readback && !sheath.microlitersPerRev,
                   "an unknown unit code keeps the code, no name; a pump never polled has no readback");
        const auto& busy = (*back.v3.pumps)[2];
        MIB_EXPECT(!busy.connected && !busy.flowRate && !busy.model, "a pump not sampled reads as unknown");
    }

    // ---- unknown is null, not zero --------------------------------------------------
    {
        rec::RunProvenanceV3 v;
        v.runId = "x";
        rec::EModulusLutRecord noLut; // loaded = false, nothing else
        v.eModulusLut = noLut;
        rec::LedRecord led;
        led.error = "the PL is not configured";
        v.led = led;
        rec::PiezoRecord piezo;
        piezo.connected = true;
        piezo.voltageV = std::numeric_limits<double>::quiet_NaN();
        v.piezo = piezo;
        rec::StageRecord stage; // not connected
        v.stage = stage;
        const std::string members = rec::runProvenanceV3Members(v);
        MIB_EXPECT(members.find("\"pumps\":null") != std::string::npos &&
                       members.find("\"pl_run_id\":null") != std::string::npos &&
                       members.find("\"voltage_v\":null") != std::string::npos &&
                       members.find("\"control\":null") != std::string::npos &&
                       members.find("\"sha256\":null") != std::string::npos,
                   "unknown facts are written as null: " + members);
        MIB_EXPECT(members.find("\"stage\":{\"connected\":false}") != std::string::npos,
                   "an absent stage writes only connected:false");
        rec::RecordedRunProvenance back;
        MIB_REQUIRE(rec::parseRunProvenance("{\"schema_version\":3," + members + "}", back), "parse the unknowns");
        MIB_EXPECT(back.v3.eModulusLut && !back.v3.eModulusLut->loaded && !back.v3.eModulusLut->sha256 &&
                       !back.v3.eModulusLut->catalogMatchesLoaded(),
                   "no LUT: no identity");
        MIB_EXPECT(back.v3.led && !back.v3.led->control && !back.v3.led->enabled() && !back.v3.led->delayUs &&
                       back.v3.led->error == std::optional<std::string>("the PL is not configured"),
                   "an unreadable LED keeps the reason and no values");
        MIB_EXPECT(back.v3.piezo && back.v3.piezo->connected && !back.v3.piezo->voltageV, "NaN voltage reads as unknown");
        MIB_EXPECT(back.v3.stage && !back.v3.stage->connected && !back.v3.stage->readback, "absent stage");
        MIB_EXPECT(!back.v3.pumps && !back.v3.plRunId, "null members read as unknown");
    }

    // ---- legacy documents --------------------------------------------------------------
    {
        rec::RecordedRunProvenance v1, v2;
        MIB_REQUIRE(rec::parseRunProvenance("{\"readiness_generation\":4,\"pixel_to_micron\":0.25}", v1), "v1 parses");
        MIB_EXPECT(v1.schemaVersion == 1 && allUnknown(v1.v3), "v1 (no schema_version) reads every v3 member as unknown");
        MIB_REQUIRE(rec::parseRunProvenance(
                        "{\"schema_version\":2,\"profile_id\":\"p\",\"method\":{\"source\":\"none\"},"
                        "\"pl_core\":{\"valid\":false},\"application\":{\"version\":\"1.1\"}}",
                        v2),
                    "v2 parses");
        MIB_EXPECT(v2.schemaVersion == 2 && allUnknown(v2.v3), "v2 reads every v3 member as unknown");
    }

    // ---- wrong types are absent; garbage is rejected --------------------------------
    {
        rec::RecordedRunProvenance back;
        MIB_REQUIRE(rec::parseRunProvenance("{\"schema_version\":3,\"run_id\":5,\"pumps\":{},\"led\":[],"
                                            "\"stage\":{\"connected\":\"yes\"},\"pl_run_id\":-4,"
                                            "\"emodulus_lut\":{\"lut_id\":\"x\"}}",
                                            back),
                    "a document with wrong member types still parses");
        MIB_EXPECT(allUnknown(back.v3), "members of the wrong type read as absent");
        std::string why;
        MIB_EXPECT(!rec::parseRunProvenance("{not json", back, &why) && !why.empty() && back.schemaVersion == 0,
                   "malformed JSON is rejected with a reason");
        MIB_EXPECT(!rec::parseRunProvenance("[1,2]", back), "a non-object is rejected");
        MIB_EXPECT(!rec::parseRunStop("{oops", &why) && !rec::parseRunStop("{\"message\":\"m\"}", &why),
                   "a malformed or reason-less stop record is rejected");
        const auto stop = rec::parseRunStop(rec::runStopToJson({"fatal_save_error", "HDF5 write failed"}));
        MIB_EXPECT(stop && stop->reason == "fatal_save_error" && stop->message == "HDF5 write failed", "stop codec");
    }

    // ---- file round trip ----------------------------------------------------------------
    const std::string path = (td / "run.h5").string();
    {
        Hdf5Service w;
        MIB_REQUIRE(w.openFile(path) && w.initializeDatasets(), "create run file");
        MIB_REQUIRE(w.writeRunSnapshotJson(snapshotDoc(full), "{\"gates\":[]}"), "write the v3 snapshot");
        MIB_REQUIRE(w.writeRunStopJson(rec::runStopToJson({"stop_requested", ""})), "write the stop record");
        w.closeFile();
        bool present = false;
        MIB_EXPECT(schemaAttribute(path, &present) == 3 && present, "run_snapshot_schema_version is 3");
        Hdf5Service r;
        MIB_REQUIRE(r.loadFile(path), "reload");
        rec::RecordedRunProvenance back;
        std::string why;
        MIB_REQUIRE(rec::readRecordedRunProvenance(r, back, &why), "typed read: " + why);
        expectEqual(full, back.v3, "file");
        MIB_EXPECT(back.stop && back.stop->reason == "stop_requested", "stop record read back");
        // Read-only: writes are refused and the file keeps what it had.
        MIB_EXPECT(!r.writeRunSnapshotJson("{\"schema_version\":3}", "{}"), "read-only file refuses the snapshot");
        MIB_EXPECT(!r.writeRunStopJson("{\"reason\":\"x\"}"), "read-only file refuses the stop record");
        rec::RecordedRunProvenance again;
        MIB_EXPECT(rec::readRecordedRunProvenance(r, again) && again.v3.runId == full.runId && again.stop &&
                       again.stop->reason == "stop_requested",
                   "refused writes changed nothing");
        r.closeFile();
    }

    // ---- no file / legacy file --------------------------------------------------------
    {
        Hdf5Service none;
        rec::RecordedRunProvenance back;
        std::string s = "stale";
        MIB_EXPECT(!none.writeRunSnapshotJson("{}", "{}") && !none.writeRunStopJson("{}"), "no file: writes refused");
        MIB_EXPECT(!none.readRunStopJson(s) && s.empty(), "no file: stop read fails and clears");
        MIB_EXPECT(!rec::readRecordedRunProvenance(none, back) && back.schemaVersion == 0 && !back.stop,
                   "no file: no provenance");

        const std::string legacy = (td / "legacy.h5").string();
        Hdf5Service w;
        MIB_REQUIRE(w.openFile(legacy) && w.initializeDatasets(), "legacy file without /run_provenance");
        w.closeFile();
        Hdf5Service r;
        MIB_REQUIRE(r.loadFile(legacy), "reload legacy");
        MIB_EXPECT(!rec::readRecordedRunProvenance(r, back) && back.schemaVersion == 0 && allUnknown(back.v3) &&
                       !back.stop,
                   "a file without run provenance: no snapshot, every fact unknown, no stop record");
        r.closeFile();
    }

    // ---- garbage and foreign attributes ---------------------------------------------
    {
        const std::string bad = (td / "garbage.h5").string();
        {
            Hdf5Service w;
            MIB_REQUIRE(w.openFile(bad) && w.initializeDatasets(), "garbage fixture");
            MIB_REQUIRE(w.writeRunSnapshotJson("{definitely not json", "{}"), "garbage snapshot stored verbatim");
            MIB_REQUIRE(w.writeRunStopJson("[\"not\",\"an\",\"object\"]"), "garbage stop stored verbatim");
            w.closeFile();
        }
        Hdf5Service r;
        MIB_REQUIRE(r.loadFile(bad), "reload garbage");
        std::string raw;
        MIB_EXPECT(r.readRunSnapshotJson(raw) && raw == "{definitely not json", "garbage reads back verbatim");
        rec::RecordedRunProvenance back;
        std::string why;
        MIB_EXPECT(!rec::readRecordedRunProvenance(r, back, &why) && !why.empty() && back.schemaVersion == 0 &&
                       allUnknown(back.v3) && !back.stop,
                   "garbage is rejected by the typed reader: no snapshot, no stop");
        r.closeFile();

        MIB_REQUIRE(writeForeignU64(bad, "run_stop_json") && writeForeignU64(bad, "run_snapshot_json"),
                    "foreign writer stores integers under the names");
        Hdf5Service f;
        MIB_REQUIRE(f.loadFile(bad), "reload foreign");
        MIB_EXPECT(!f.readRunStopJson(raw) && !f.readRunSnapshotJson(raw), "non-string attributes read as absent");
        f.closeFile();
    }

    // ---- a snapshot the file cannot hold ----------------------------------------------
    {
        const std::string big = (td / "oversized.h5").string();
        Hdf5Service w;
        MIB_REQUIRE(w.openFile(big) && w.initializeDatasets(), "oversized fixture");
        const std::string doc = "{\"schema_version\":3,\"profile_id\":\"" + std::string(70 * 1024, 'p') + "\"}";
        const bool wrote = w.writeRunSnapshotJson(doc, "{}");
        w.closeFile();
        bool present = false;
        const uint64_t version = schemaAttribute(big, &present);
        Hdf5Service r;
        MIB_REQUIRE(r.loadFile(big), "reload oversized");
        std::string raw;
        if (wrote) {
            MIB_EXPECT(r.readRunSnapshotJson(raw) && raw == doc && present && version == 3,
                       "this HDF5 stores a large snapshot whole");
        } else {
            MIB_EXPECT(!r.readRunSnapshotJson(raw) && !present,
                       "a refused snapshot reports failure and leaves no version attribute promising one");
        }
        r.closeFile();
    }

    return mib::test::exitCode();
}
