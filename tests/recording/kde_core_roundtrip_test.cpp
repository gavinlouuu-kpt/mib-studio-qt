// kde_core_roundtrip_test
//
// Save-data round-trip for the KDE core contour records (coverage matrix:
// Save data -> round-trip). Both records written into a real experiment
// file survive close/reopen byte-identical; the codec reproduces every
// field and every contour vertex; a file without records reads as "none";
// the live and analysis records are independent (writing one never touches
// the other); rewriting a record replaces it.

#include "backend/recording/Hdf5Service.h"
#include "backend/processing/KdeCoreRecord.h"

#include "support/assert.h"
#include "support/tempdir.h"

#include <cmath>
#include <string>

using backend::services::Hdf5Service;
using backend::monitoring::fromJson;
using backend::monitoring::KdeCoreRecord;
using backend::monitoring::toJson;

namespace {

KdeCoreRecord sample(bool provisional) {
    KdeCoreRecord r;
    r.provisional = provisional;
    r.source = provisional ? "live-buffer" : "full-run";
    r.coreFraction = 0.9;
    r.level = 0.137;
    r.cellCount = 612;
    r.populationCount = 1000;
    r.excludedPoints = provisional ? 0 : 3;
    r.bandwidthFactor = provisional ? 1.5 : 1.0;
    r.bandwidthX = 21.4;
    r.bandwidthY = 0.0049;
    r.pixelToMicron = 0.4886;
    r.x0 = 0.0;
    r.x1 = 1000.0;
    r.y0 = 0.0;
    r.y1 = 1.0;
    r.gridNx = 128;
    r.gridNy = 64;
    r.computedAtNs = 1790000000123456789ULL;
    // Two loops (bimodal), the second small, values with many digits.
    r.contours.push_back(
        {{150.125, 0.031}, {220.5, 0.029}, {230.0, 0.071}, {160.75, 0.080}, {150.125, 0.031}});
    r.contours.push_back({{400.0, 0.3}, {420.0, 0.3}, {410.0, 0.3333333333333333}, {400.0, 0.3}});
    return r;
}

bool sameRecord(const KdeCoreRecord& a, const KdeCoreRecord& b) {
    if (a.provisional != b.provisional || a.source != b.source ||
        a.coreFraction != b.coreFraction || a.level != b.level || a.cellCount != b.cellCount ||
        a.populationCount != b.populationCount || a.excludedPoints != b.excludedPoints ||
        a.bandwidthFactor != b.bandwidthFactor || a.bandwidthX != b.bandwidthX ||
        a.bandwidthY != b.bandwidthY || a.pixelToMicron != b.pixelToMicron || a.x0 != b.x0 ||
        a.x1 != b.x1 || a.y0 != b.y0 || a.y1 != b.y1 || a.gridNx != b.gridNx ||
        a.gridNy != b.gridNy || a.computedAtNs != b.computedAtNs ||
        a.contours.size() != b.contours.size())
        return false;
    for (std::size_t i = 0; i < a.contours.size(); ++i) {
        if (a.contours[i].size() != b.contours[i].size()) return false;
        for (std::size_t k = 0; k < a.contours[i].size(); ++k)
            if (a.contours[i][k].x != b.contours[i][k].x ||
                a.contours[i][k].y != b.contours[i][k].y)
                return false;
    }
    return true;
}

} // namespace

int main() {
    mib::test::TempDir td("mib_kde_core_roundtrip");
    const std::string path = (td / "experiment.h5").string();
    const KdeCoreRecord live = sample(true);
    const KdeCoreRecord analysis = sample(false);
    const std::string liveJson = toJson(live);
    const std::string analysisJson = toJson(analysis);

    // ---- codec round-trip (no file) ------------------------------------------
    {
        const auto back = fromJson(liveJson);
        MIB_REQUIRE(back.has_value(), "codec parses its own output");
        MIB_EXPECT(sameRecord(*back, live), "codec reproduces every field and vertex exactly");
        KdeCoreRecord none = live;
        none.level = std::nan("");
        none.contours.clear();
        const auto noneBack = fromJson(toJson(none));
        MIB_REQUIRE(noneBack.has_value(), "a record without a contour is valid");
        MIB_EXPECT(std::isnan(noneBack->level) && noneBack->contours.empty(),
                   "no level / no contour survives as NaN / empty");
    }

    // ---- file without records -------------------------------------------------
    {
        Hdf5Service hdf5;
        MIB_REQUIRE(hdf5.openFile(path), "openFile");
        MIB_REQUIRE(hdf5.initializeDatasets(), "initializeDatasets");
        std::string s = "stale";
        MIB_EXPECT(!hdf5.readKdeLiveJson(s) && s.empty(), "fresh file: no live record");
        MIB_EXPECT(!hdf5.readKdeAnalysisJson(s) && s.empty(), "fresh file: no analysis record");
        // Write only the live record first.
        MIB_REQUIRE(hdf5.writeKdeLiveJson(liveJson), "write live record");
        hdf5.closeFile();
    }
    {
        Hdf5Service reader;
        MIB_REQUIRE(reader.loadFile(path), "reopen read-only");
        std::string s;
        MIB_EXPECT(reader.readKdeLiveJson(s) && s == liveJson,
                   "live record survives close/reopen byte-identical");
        MIB_EXPECT(!reader.readKdeAnalysisJson(s),
                   "writing the live record does not create an analysis record");
        reader.closeFile();
    }

    // ---- both records, rewrite replaces --------------------------------------
    const std::string path2 = (td / "experiment2.h5").string();
    {
        Hdf5Service hdf5;
        MIB_REQUIRE(hdf5.openFile(path2), "openFile 2");
        MIB_REQUIRE(hdf5.initializeDatasets(), "initializeDatasets 2");
        MIB_REQUIRE(hdf5.writeKdeAnalysisJson("{\"schema_version\":1,\"contours\":[]}"),
                    "write placeholder analysis");
        MIB_REQUIRE(hdf5.writeKdeAnalysisJson(analysisJson), "rewrite analysis record");
        MIB_REQUIRE(hdf5.writeKdeLiveJson(liveJson), "write live record");
        hdf5.closeFile();
    }
    {
        Hdf5Service reader;
        MIB_REQUIRE(reader.loadFile(path2), "reopen 2");
        std::string l, a;
        MIB_EXPECT(reader.readKdeLiveJson(l) && l == liveJson,
                   "live record intact next to the analysis record");
        MIB_EXPECT(reader.readKdeAnalysisJson(a) && a == analysisJson,
                   "rewrite replaced the analysis record");
        const auto parsedA = fromJson(a);
        const auto parsedL = fromJson(l);
        MIB_REQUIRE(parsedA && parsedL, "both parse");
        MIB_EXPECT(!parsedA->provisional && parsedA->source == "full-run" &&
                       sameRecord(*parsedA, analysis),
                   "analysis record read back as the full-run, non-provisional record");
        MIB_EXPECT(parsedL->provisional && parsedL->source == "live-buffer" &&
                       sameRecord(*parsedL, live),
                   "live record read back as provisional");
        reader.closeFile();
    }

    return mib::test::exitCode();
}
