// kde_core_fault_test
//
// Fault injection for the KDE core contour records (coverage matrix:
// Save data -> fault-injection):
//  - writes are refused, explicitly, with no file open and on a file opened
//    read-only (the file is left untouched);
//  - reads with no file open fail cleanly;
//  - the codec rejects malformed JSON, non-objects, a missing or future
//    schema version, and malformed contours, with a reason, never throwing;
//  - a garbage document stored in the attribute reads back verbatim and is
//    then rejected by the codec (readers treat it as "no record");
//  - a non-string attribute of the same name (foreign writer) reads as absent.

#include "backend/recording/Hdf5Service.h"
#include "frontend/tabs/KdeCoreRecord.h"

#include "support/assert.h"
#include "support/tempdir.h"

#include <hdf5.h>

#include <string>

using backend::services::Hdf5Service;
using frontend::monitoring::fromJson;
using frontend::monitoring::KdeCoreRecord;
using frontend::monitoring::toJson;

int main() {
    mib::test::TempDir td("mib_kde_core_fault");
    const std::string path = (td / "experiment.h5").string();
    const std::string good = toJson(KdeCoreRecord{});

    // ---- no file open ---------------------------------------------------------
    {
        Hdf5Service hdf5;
        std::string s = "stale";
        MIB_EXPECT(!hdf5.writeKdeLiveJson(good) && !hdf5.writeKdeAnalysisJson(good),
                   "no file: writes refused");
        MIB_EXPECT(!hdf5.readKdeLiveJson(s) && s.empty(),
                   "no file: read fails and clears the output");
        MIB_EXPECT(!hdf5.readKdeAnalysisJson(s) && s.empty(), "no file: analysis read fails");
    }

    // ---- read-only file refuses writes ---------------------------------------
    {
        Hdf5Service hdf5;
        MIB_REQUIRE(hdf5.openFile(path), "create file");
        MIB_REQUIRE(hdf5.initializeDatasets(), "datasets");
        hdf5.closeFile();
    }
    {
        Hdf5Service reader;
        MIB_REQUIRE(reader.loadFile(path), "open read-only");
        MIB_EXPECT(!reader.writeKdeLiveJson(good), "read-only: live write refused");
        MIB_EXPECT(!reader.writeKdeAnalysisJson(good), "read-only: analysis write refused");
        std::string s;
        MIB_EXPECT(!reader.readKdeLiveJson(s) && !reader.readKdeAnalysisJson(s),
                   "refused writes left nothing behind");
        reader.closeFile();
    }

    // ---- codec rejects bad documents -----------------------------------------
    {
        std::string why;
        MIB_EXPECT(!fromJson("{not json", &why) && !why.empty(),
                   "malformed JSON rejected with a reason");
        MIB_EXPECT(!fromJson("[1,2,3]", &why), "non-object rejected");
        MIB_EXPECT(!fromJson("{\"contours\":[]}", &why) &&
                       why.find("schema_version") != std::string::npos,
                   "missing schema_version rejected");
        MIB_EXPECT(!fromJson("{\"schema_version\":99,\"contours\":[]}", &why) &&
                       why.find("99") != std::string::npos,
                   "future schema version rejected, naming the version");
        MIB_EXPECT(!fromJson("{\"schema_version\":1}", &why), "missing contours rejected");
        MIB_EXPECT(!fromJson("{\"schema_version\":1,\"contours\":[[[1,2],[3]]]}", &why),
                   "vertex without two numbers rejected");
        MIB_EXPECT(!fromJson("{\"schema_version\":1,\"contours\":[[[1,\"x\"]]]}", &why),
                   "non-numeric vertex rejected");
        MIB_EXPECT(
            !fromJson("{\"schema_version\":1,\"cell_count\":\"many\",\"contours\":[]}", &why),
            "wrongly typed member rejected, not thrown");
        MIB_EXPECT(!fromJson("", &why), "empty document rejected");
        const auto degenerate = fromJson("{\"schema_version\":1,\"contours\":[[[1,2],[3,4]]]}");
        MIB_EXPECT(degenerate && degenerate->contours.empty(),
                   "loops with fewer than 3 vertices are dropped");
        const auto extra =
            fromJson("{\"schema_version\":1,\"contours\":[],\"future_member\":{\"a\":1}}");
        MIB_EXPECT(extra.has_value(), "unknown extra members are ignored");
    }

    // ---- garbage stored in the attribute --------------------------------------
    const std::string path2 = (td / "garbage.h5").string();
    {
        Hdf5Service hdf5;
        MIB_REQUIRE(hdf5.openFile(path2), "create file 2");
        MIB_REQUIRE(hdf5.initializeDatasets(), "datasets 2");
        MIB_REQUIRE(hdf5.writeKdeLiveJson("\xff\xfe garbage \x01"),
                    "service stores the document verbatim");
        hdf5.closeFile();
    }
    {
        Hdf5Service reader;
        MIB_REQUIRE(reader.loadFile(path2), "reopen 2");
        std::string s;
        MIB_EXPECT(reader.readKdeLiveJson(s) && s == "\xff\xfe garbage \x01",
                   "garbage reads back verbatim");
        MIB_EXPECT(!fromJson(s), "garbage is rejected by the codec");
        reader.closeFile();
    }

    // ---- foreign, non-string attribute of the same name ----------------------
    const std::string path3 = (td / "foreign.h5").string();
    {
        hid_t file = H5Fcreate(path3.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
        MIB_REQUIRE(file >= 0, "raw create");
        hid_t group = H5Gcreate2(file, "/monitoring", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        hid_t scalar = H5Screate(H5S_SCALAR);
        hid_t attr =
            H5Acreate2(group, "kde_live_json", H5T_NATIVE_INT, scalar, H5P_DEFAULT, H5P_DEFAULT);
        const int value = 42;
        H5Awrite(attr, H5T_NATIVE_INT, &value);
        H5Aclose(attr);
        H5Sclose(scalar);
        H5Gclose(group);
        H5Fclose(file);
    }
    {
        Hdf5Service reader;
        MIB_REQUIRE(reader.loadFile(path3), "open foreign file");
        std::string s;
        MIB_EXPECT(!reader.readKdeLiveJson(s) && s.empty(), "non-string attribute reads as absent");
        reader.closeFile();
    }

    return mib::test::exitCode();
}
