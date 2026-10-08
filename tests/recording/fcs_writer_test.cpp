#include "backend/recording/FcsWriter.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/processing/ProcessingService.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <array>
#include <optional>
#include <string>
#include <vector>
#include <algorithm>
#include <cstring>
#include <map>
#include <locale>
#include <hdf5.h>

namespace fs = std::filesystem;
using backend::recording::FcsWriteOptions;
using backend::services::ProcessedFrame;

namespace {

class CommaLocale : public std::numpunct<char> {
    char do_decimal_point() const override { return ','; }
    char do_thousands_sep() const override { return '_'; }
    std::string do_grouping() const override { return "\3"; }
};

struct ParsedFcs {
    std::string bytes;
    std::map<std::string, std::string> text;
    uint64_t dataStart{0}, dataEnd{0};

    float value(size_t index) const {
        const size_t offset = static_cast<size_t>(dataStart) + index * 4;
        MIB_REQUIRE(offset + 4 <= bytes.size(), "float lies within DATA");
        uint32_t bits = 0;
        for (unsigned byte = 0; byte < 4; ++byte)
            bits |= static_cast<uint32_t>(static_cast<unsigned char>(bytes[offset + byte]))
                    << (byte * 8);
        float decoded;
        std::memcpy(&decoded, &bits, sizeof(decoded));
        return decoded;
    }
};

ParsedFcs parseFcs(const fs::path& path) {
    ParsedFcs parsed;
    std::ifstream input(path, std::ios::binary);
    parsed.bytes.assign(std::istreambuf_iterator<char>(input), {});
    MIB_REQUIRE(parsed.bytes.size() >= 58, "complete FCS header");
    MIB_EXPECT(parsed.bytes.substr(0, 6) == "FCS3.1", "FCS version");
    const auto offset = [&](size_t position) {
        return std::stoull(parsed.bytes.substr(position, 8));
    };
    const uint64_t textStart = offset(10), textEnd = offset(18);
    parsed.dataStart = offset(26);
    parsed.dataEnd = offset(34);
    MIB_REQUIRE(textStart == 58 && textEnd >= textStart && textEnd < parsed.bytes.size(),
                "TEXT offsets");
    MIB_EXPECT(offset(42) == 0 && offset(50) == 0, "no ANALYSIS segment");
    const std::string text = parsed.bytes.substr(textStart, textEnd - textStart + 1);
    const char delimiter = text.front();
    std::vector<std::string> tokens;
    size_t position = 1;
    while (position < text.size()) {
        std::string token;
        while (position < text.size()) {
            const char c = text[position++];
            if (c == delimiter) {
                if (position < text.size() && text[position] == delimiter)
                    ++position;
                else
                    break;
            }
            token.push_back(c);
        }
        tokens.push_back(token);
    }
    MIB_REQUIRE(tokens.size() % 2 == 0, "TEXT consists of keyword/value pairs");
    for (size_t i = 0; i < tokens.size(); i += 2)
        MIB_REQUIRE(parsed.text.emplace(tokens[i], tokens[i + 1]).second, "unique TEXT keywords");
    MIB_EXPECT(parsed.dataStart == textEnd + 1, "DATA follows TEXT");
    MIB_EXPECT(std::stoull(parsed.text.at("$BEGINDATA")) == parsed.dataStart &&
                   std::stoull(parsed.text.at("$ENDDATA")) == parsed.dataEnd,
               "HEADER/TEXT DATA offsets agree");
    const uint64_t events = std::stoull(parsed.text.at("$TOT"));
    const uint64_t parameters = std::stoull(parsed.text.at("$PAR"));
    MIB_EXPECT(parsed.dataEnd + 1 == parsed.bytes.size(), "DATA end is inclusive");
    MIB_EXPECT(parsed.bytes.size() - parsed.dataStart == events * parameters * 4,
               "DATA length matches TOT/PAR");
    MIB_EXPECT(parsed.text.at("$MODE") == "L" && parsed.text.at("$DATATYPE") == "F" &&
                   parsed.text.at("$BYTEORD") == "1,2,3,4",
               "list-mode little-endian float data");
    MIB_EXPECT(parsed.text.at("$BEGINANALYSIS") == "0" && parsed.text.at("$ENDANALYSIS") == "0" &&
                   parsed.text.at("$BEGINSTEXT") == "0" && parsed.text.at("$ENDSTEXT") == "0",
               "absent optional segments");
    for (size_t parameter = 1; parameter <= parameters; ++parameter) {
        const std::string p = "$P" + std::to_string(parameter);
        MIB_EXPECT(parsed.text.at(p + "B") == "32" && parsed.text.at(p + "E") == "0,0",
                   "linear float parameter");
        MIB_EXPECT(!parsed.text.at(p + "N").empty() && !parsed.text.at(p + "S").empty(),
                   "parameter names/labels");
        const double range = std::stod(parsed.text.at(p + "R"));
        MIB_EXPECT(std::isfinite(range) && range > 0, "finite positive range");
        for (size_t event = 0; event < events; ++event)
            MIB_EXPECT(std::abs(parsed.value(event * parameters + parameter - 1)) < range,
                       "range encloses DATA value");
    }
    return parsed;
}

ProcessedFrame fixtureFrame(uint64_t index, uint64_t timestamp, bool valid, int contract,
                            int ordinal) {
    ProcessedFrame frame;
    frame.index = index;
    frame.timestampNs = timestamp;
    frame.originalImage = cv::Mat(2, 2, CV_8UC1, cv::Scalar(ordinal + 1));
    frame.processedImage = cv::Mat(2, 2, CV_8UC1, cv::Scalar(0));
    frame.validation.isValid = valid;
    frame.validation.objectId = 7;
    frame.validation.area = ordinal == 0 ? 100.25 : ordinal == 1 ? 200.5 : 50.0;
    frame.validation.deformability = ordinal == 0 ? .125 : ordinal == 1 ? .25 : .5;
    frame.validation.areaRatio = 1.1 + ordinal * .1;
    frame.validation.ringRatio = .5 + ordinal * .1;
    frame.validation.laplacianVariance = 42.0 * (ordinal + 1);
    frame.validation.brightness.q1 = 10.0 + ordinal;
    frame.validation.brightness.q2 = 20.0 + ordinal;
    frame.validation.brightness.q3 = 30.0 + ordinal;
    frame.validation.brightness.q4 = 40.0 + ordinal;
    frame.validation.brightnessMean = 112.5 + ordinal;
    frame.validation.brightnessVariance = 33.25 + ordinal;
    frame.validation.pixelCount = 250 + ordinal;
    frame.validation.blemishCount = ordinal;
    (void)contract;
    return frame;
}

bool writeFlowFixtures(const fs::path& root) {
    fs::create_directories(root);
    const uint64_t base = 1700000000000000000ULL;
    const std::vector<std::string> names{"index",
                                         "timestampNs",
                                         "area",
                                         "deformability",
                                         "areaRatio",
                                         "ringRatio",
                                         "laplacianVariance",
                                         "brightness_q1",
                                         "brightness_q2",
                                         "brightness_q3",
                                         "brightness_q4",
                                         "brightness_mean",
                                         "brightness_variance",
                                         "pixelCount",
                                         "blemishCount"};
    for (int contract = 1; contract <= 3; ++contract) {
        std::vector<ProcessedFrame> valid{
            fixtureFrame(42, base, true, contract, 0),
            fixtureFrame(9007199254740993ULL, base + 1000000000ULL, true, contract, 1)};
        std::vector<ProcessedFrame> invalid{
            fixtureFrame(8, base + 2000000000ULL, false, contract, 2)};
        const fs::path path = root / ("contract" + std::to_string(contract) + ".h5");
        backend::services::Hdf5Service service;
        if (!service.openFile(path.string()) || !service.initializeDatasets() ||
            !service.appendFrames(valid, invalid))
            return false;
        backend::services::ProcessingConfig config;
        config.processing_contract_version = contract;
        backend::processing::ProcessingCoreIdentity identity;
        identity.contractVersion = static_cast<uint32_t>(contract);
        identity.source = "fixture";
        backend::services::ProcessingService::Roi roi{0, 0, 2, 2};
        if (!service.writeExperimentInfo(base, base + 2000000000ULL, valid.size(), invalid.size(),
                                         config, roi, nullptr, &identity))
            return false;
        service.closeFile();
    }
    {
        const fs::path path = root / "empty.h5";
        backend::services::Hdf5Service service;
        if (!service.openFile(path.string()) || !service.initializeDatasets()) return false;
        backend::services::ProcessingConfig config;
        config.processing_contract_version = 1;
        backend::services::ProcessingService::Roi roi{0, 0, 2, 2};
        if (!service.writeExperimentInfo(base, base, 0, 0, config, roi)) return false;
        service.closeFile();
        hid_t source =
            H5Fopen((root / "contract1.h5").string().c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
        hid_t destination = H5Fopen(path.string().c_str(), H5F_ACC_RDWR, H5P_DEFAULT);
        bool ok = source >= 0 && destination >= 0 &&
                  H5Ocopy(source, "/valid_frames/metadata", destination, "/valid_frames/metadata",
                          H5P_DEFAULT, H5P_DEFAULT) >= 0;
        hid_t dataset = ok ? H5Dopen2(destination, "/valid_frames/metadata", H5P_DEFAULT) : -1;
        const hsize_t zero = 0;
        ok = dataset >= 0 && H5Dset_extent(dataset, &zero) >= 0;
        if (dataset >= 0) H5Dclose(dataset);
        if (destination >= 0) H5Fclose(destination);
        if (source >= 0) H5Fclose(source);
        if (!ok) return false;
    }
    return true;
}

bool writeContractFixture(const fs::path& path, bool array, int rootValue,
                          std::optional<int> experimentValue = {}) {
    hid_t file = H5Fcreate(path.string().c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    if (file < 0) return false;
    hid_t root = H5Gopen2(file, "/", H5P_DEFAULT);
    hid_t space = H5Screate_simple(array ? 1 : 0,
                                   array ? std::array<hsize_t, 1>{2}.data() : nullptr, nullptr);
    if (root < 0 || space < 0) return false;
    hid_t attr = H5Acreate2(root, "processing_contract_version", H5T_NATIVE_INT32, space,
                            H5P_DEFAULT, H5P_DEFAULT);
    int values[2] = {rootValue, rootValue};
    const bool rootOk = attr >= 0 && H5Awrite(attr, H5T_NATIVE_INT32, values) >= 0;
    if (attr >= 0) H5Aclose(attr);
    H5Sclose(space);
    H5Gclose(root);
    bool experimentOk = true;
    if (experimentValue) {
        hid_t group = H5Gcreate2(file, "/experiment_info", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        hid_t scalar = H5Screate(H5S_SCALAR);
        hid_t a = group >= 0 && scalar >= 0
                      ? H5Acreate2(group, "processing_contract_version", H5T_NATIVE_INT32, scalar,
                                   H5P_DEFAULT, H5P_DEFAULT)
                      : -1;
        const int value = *experimentValue;
        experimentOk = a >= 0 && H5Awrite(a, H5T_NATIVE_INT32, &value) >= 0;
        if (a >= 0) H5Aclose(a);
        if (scalar >= 0) H5Sclose(scalar);
        if (group >= 0) H5Gclose(group);
    }
    H5Fclose(file);
    return rootOk && experimentOk;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--fixtures")
        return writeFlowFixtures(argv[2]) ? 0 : 1;
    mib::test::Watchdog wd(30);
    mib::test::TempDir td("fcs_writer");
    const fs::path outputRoot =
        std::getenv("MIB_FCS_KEEP_DIR") ? fs::path(std::getenv("MIB_FCS_KEEP_DIR")) : td.path();
    fs::create_directories(outputRoot);
    std::vector<ProcessedFrame> rows(2);
    rows[0].index = 7;
    rows[0].timestampNs = 2000000000ULL;
    rows[0].validation.objectId = 7;
    rows[0].validation.area = 12.0;
    rows[0].validation.deformability = 0.25;
    rows[1].index = 3;
    rows[1].timestampNs = 1000000000ULL;
    rows[1].validation.objectId = 3;
    rows[1].validation.area = 13.0;
    rows[1].validation.deformability = 0.5;
    const std::vector<std::string> members{"index", "timestampNs", "area", "deformability",
                                           "ringRatio"};
    FcsWriteOptions options;
    options.contract = 1;
    options.pixelToMicron = 0.5;
    options.metadata.source = "/tmp/rows.h5";
    options.metadata.contract = 1;
    options.metadata.exportTime = "2026-10-07T00:00:00Z";
    options.metadata.mibVersion = "test";
    options.metadata.pixelToMicron = 0.5;
    const auto path = outputRoot / "rows.fcs";
    const auto map = outputRoot / "rows_event_map.csv";
    const auto result =
        backend::recording::writeFcs(path.string(), map.string(), rows, members, options);
    MIB_REQUIRE(result.error.empty(), "writer result: " + result.error);
    MIB_EXPECT(result.events == 2 && result.fields.size() == 5, "contract-aware field count");
    std::ifstream input(path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(input)), {});
    MIB_EXPECT(bytes.substr(0, 6) == "FCS3.1", "FCS version");
    MIB_EXPECT(bytes.find("$MODE|L|") != std::string::npos &&
                   bytes.find("$DATATYPE|F|") != std::string::npos,
               "FCS mode and datatype");
    MIB_EXPECT(bytes.find("$BYTEORD|1,2,3,4|") != std::string::npos &&
                   bytes.find("$TIMESTEP|1|") != std::string::npos,
               "FCS byte order and timestep");
    MIB_EXPECT(bytes.find("$FIL|rows.h5|") != std::string::npos, "source filename metadata");
    MIB_EXPECT(bytes.find("$P1E|0,0|") != std::string::npos &&
                   bytes.find("$P1R|") != std::string::npos,
               "parameter metadata");
    std::ifstream eventMap(map);
    std::string line;
    std::getline(eventMap, line);
    MIB_EXPECT(line == "fcs_event_index,source_frame_index,object_id,timestamp_ns,event_mode",
               "event map schema");
    std::getline(eventMap, line);
    MIB_EXPECT(line.find("0,3,3,1000000000,detection") != std::string::npos,
               "stable temporal event order");
    const auto parsed = parseFcs(path);
    MIB_EXPECT(parsed.text.at("$TOT") == "2" && parsed.text.at("$PAR") == "5",
               "event/parameter counts");
    const std::array<float, 10> expected{3.25f, 13.f, .5f, 0.f, 0.f, 3.f, 12.f, .25f, 0.f, 1.f};
    for (size_t i = 0; i < expected.size(); ++i)
        MIB_EXPECT(parsed.value(i) == expected[i], "float32 values round-trip in temporal order");
    MIB_EXPECT(parsed.text.at("$P1N") == "Area_um2" && parsed.text.at("$P2N") == "Area_px2" &&
                   parsed.text.at("$P5N") == "Time" && parsed.text.at("$P5U") == "s" &&
                   parsed.text.at("$TIMESTEP") == "1",
               "canonical names and seconds");
    MIB_EXPECT(std::stod(parsed.text.at("$P2R")) == 14.0, "range derives from serialized values");

    const auto smallLayout = backend::recording::planFcsLayout(100, 8);
    MIB_EXPECT(backend::recording::formatFcsHeader(smallLayout).substr(10, 8) == "00000058",
               "text header offset");
    const auto hugeLayout = backend::recording::planFcsLayout(100, 100000000);
    const auto hugeHeader = backend::recording::formatFcsHeader(hugeLayout);
    MIB_EXPECT(hugeHeader.substr(26, 16) == "0000000000000000",
               "data offsets are zero when beyond legacy header range");
    MIB_EXPECT(hugeHeader.substr(10, 16) == "0000005800000157" && hugeLayout.beginData == 158 &&
                   hugeLayout.endData == 100000157,
               "TEXT offsets remain populated when only DATA end overflows");
    MIB_EXPECT(
        backend::recording::planFcsLayout(std::numeric_limits<uint64_t>::max(), 8).overflow &&
            backend::recording::planFcsLayout(100, std::numeric_limits<uint64_t>::max()).overflow,
        "layout arithmetic rejects uint64 overflow");

    rows[0].validation.laplacianVariance = std::numeric_limits<double>::quiet_NaN();
    const auto bad = backend::recording::writeFcs(
        (outputRoot / "bad.fcs").string(), (outputRoot / "bad.csv").string(), rows,
        {"index", "timestampNs", "laplacianVariance"}, FcsWriteOptions{2});
    MIB_EXPECT(!bad.error.empty() && bad.error.find("non-finite") != std::string::npos,
               "non-finite fails closed");
    const auto empty =
        backend::recording::writeFcs((outputRoot / "empty.fcs").string(),
                                     (outputRoot / "empty.csv").string(), {}, members, options);
    MIB_EXPECT(empty.error.empty() && empty.events == 0, "empty FCS is valid");
    const auto parsedEmpty = parseFcs(outputRoot / "empty.fcs");
    MIB_EXPECT(parsedEmpty.text.at("$TOT") == "0" &&
                   parsedEmpty.dataEnd + 1 == parsedEmpty.dataStart,
               "zero-event DATA has zero bytes");

    const std::vector<std::string> allMembers{"index",
                                              "timestampNs",
                                              "area",
                                              "deformability",
                                              "areaRatio",
                                              "ringRatio",
                                              "laplacianVariance",
                                              "brightness_q1",
                                              "brightness_q2",
                                              "brightness_q3",
                                              "brightness_q4",
                                              "brightness_mean",
                                              "brightness_variance",
                                              "pixelCount",
                                              "blemishCount"};
    const uint64_t base = 1700000000000000000ULL;
    for (int contract = 1; contract <= 3; ++contract) {
        const std::vector<ProcessedFrame> detections{
            fixtureFrame(9007199254740993ULL, base + 1000000000ULL, true, contract, 1),
            fixtureFrame(42, base, true, contract, 0)};
        FcsWriteOptions contractOptions = options;
        contractOptions.contract = contract;
        contractOptions.metadata.contract = contract;
        contractOptions.metadata.source = "folder/run|sample.h5";
        contractOptions.metadata.runId = "run|1";
        const auto contractPath =
            outputRoot / ("writer-contract" + std::to_string(contract) + ".fcs");
        const auto contractMap =
            outputRoot / ("writer-contract" + std::to_string(contract) + ".csv");
        const auto written = backend::recording::writeFcs(
            contractPath.string(), contractMap.string(), detections, allMembers, contractOptions);
        MIB_REQUIRE(written.error.empty(), "contract writer: " + written.error);
        const auto record = parseFcs(contractPath);
        const std::vector<std::string> names =
            contract == 1
                ? std::vector<std::string>{"Area_um2",  "Area_px2", "Deformability", "AreaRatio",
                                           "RingRatio", "BrightQ1", "BrightQ2",      "BrightQ3",
                                           "BrightQ4",  "Time"}
            : contract == 2
                ? std::vector<std::string>{"Area_um2",     "Area_px2", "Deformability", "AreaRatio",
                                           "LaplacianVar", "BrightQ1", "BrightQ2",      "BrightQ3",
                                           "BrightQ4",     "Time"}
                : std::vector<std::string>{
                      "Area_um2",   "Area_px2",  "Deformability", "AreaRatio", "LaplacianVar",
                      "BrightMean", "BrightVar", "Pixels",        "Blemishes", "Time"};
        MIB_EXPECT(record.text.at("$PAR") == "10" && written.events == 2,
                   "duplicate object IDs retain both events");
        for (size_t p = 0; p < names.size(); ++p)
            MIB_EXPECT(record.text.at("$P" + std::to_string(p + 1) + "N") == names[p],
                       "contract parameter names/order");
        MIB_EXPECT(record.value(0) == 25.0625f && record.value(10) == 50.125f &&
                       record.value(9) == 0.f && record.value(19) == 1.f,
                   "calibration and relative time");
        MIB_EXPECT(record.text.at("$MIB_SOURCE") == "folder/run|sample.h5" &&
                       record.text.at("$MIB_RUN_ID") == "run|1" &&
                       record.text.at("$FIL") == "run|sample.h5",
                   "TEXT delimiter escaping preserves source/run metadata");
        std::ifstream sidecar(contractMap);
        std::getline(sidecar, line);
        std::getline(sidecar, line);
        MIB_EXPECT(line == "0,42,7,1700000000000000000,detection",
                   "exact first source observation");
        std::getline(sidecar, line);
        MIB_EXPECT(line == "1,9007199254740993,7,1700000001000000000,detection",
                   "exact uint64 traceability above float precision");
    }
    const auto missingFields =
        backend::recording::fcsFieldRegistry(3, {"area", "timestampNs", "brightness_mean"});
    MIB_EXPECT(missingFields.size() == 4 && std::none_of(missingFields.begin(), missingFields.end(),
                                                         [](const auto& field) {
                                                             return field.pnn == "Pixels" ||
                                                                    field.pnn == "Blemishes" ||
                                                                    field.pnn == "BrightQ1";
                                                         }),
               "absent and contract-inapplicable fields never become channels");
    for (double value : {std::numeric_limits<double>::infinity(),
                         static_cast<double>(std::numeric_limits<float>::max()) * 2}) {
        rows[0].validation.laplacianVariance = value;
        const auto rejected = backend::recording::writeFcs(
            (outputRoot / "invalid.fcs").string(), (outputRoot / "invalid.csv").string(), rows,
            {"timestampNs", "laplacianVariance"}, FcsWriteOptions{2});
        MIB_EXPECT(!rejected.error.empty(), "Inf and float32 overflow fail closed");
    }
    std::atomic<bool> cancelled{true};
    auto cancelOptions = options;
    cancelOptions.cancel = &cancelled;
    const auto cancelledWrite = backend::recording::writeFcs(
        (outputRoot / "cancelled.fcs").string(), (outputRoot / "cancelled.csv").string(), rows,
        members, cancelOptions);
    MIB_EXPECT(cancelledWrite.cancelled && !fs::exists(outputRoot / "cancelled.fcs"),
               "pre-cancelled writer creates no artifact");
    const auto priorLocale = std::locale();
    std::locale::global(std::locale(priorLocale, new CommaLocale));
    const auto localeResult =
        backend::recording::writeFcs((outputRoot / "locale.fcs").string(),
                                     (outputRoot / "locale.csv").string(), rows, members, options);
    std::locale::global(priorLocale);
    MIB_REQUIRE(localeResult.error.empty(), "locale-independent writer");
    const auto localeRecord = parseFcs(outputRoot / "locale.fcs");
    MIB_EXPECT(localeRecord.text.at("$MIB_PIXEL_TO_MICRON") == "0.5",
               "protocol numeric metadata uses decimal point");
    std::ifstream localeMap(outputRoot / "locale.csv");
    std::getline(localeMap, line);
    std::getline(localeMap, line);
    MIB_EXPECT(line == "0,3,3,1000000000,detection", "CSV integers never contain locale grouping");

    // Contract provenance is untrusted HDF5 input: arrays, unsupported
    // values, and conflicting copies fail closed; absent/partial legacy
    // metadata remains the safe Contract-1 interpretation.
    const auto checkContract = [&](const fs::path& p, bool expectedOk, int expected) {
        backend::services::Hdf5Service reader;
        MIB_REQUIRE(reader.loadFile(p.string()), "open contract fixture");
        int contract = 0;
        const bool ok = reader.readRecordedProcessingContract(contract);
        MIB_EXPECT(ok == expectedOk && (!ok || contract == expected),
                   "contract provenance validation");
    };
    const fs::path missing = outputRoot / "contract_missing.h5";
    const fs::path partial = outputRoot / "contract_partial.h5";
    const fs::path malformed = outputRoot / "contract_array.h5";
    const fs::path conflict = outputRoot / "contract_conflict.h5";
    const fs::path unknown = outputRoot / "contract_unknown.h5";
    MIB_REQUIRE(writeContractFixture(missing, false, 1), "write missing contract fixture");
    // Remove the sole attribute to model a legacy file.
    {
        hid_t f = H5Fopen(missing.string().c_str(), H5F_ACC_RDWR, H5P_DEFAULT);
        hid_t g = H5Gopen2(f, "/", H5P_DEFAULT);
        H5Adelete(g, "processing_contract_version");
        H5Gclose(g);
        H5Fclose(f);
    }
    MIB_REQUIRE(writeContractFixture(partial, false, 2), "write partial contract fixture");
    MIB_REQUIRE(writeContractFixture(malformed, true, 2), "write malformed contract fixture");
    MIB_REQUIRE(writeContractFixture(conflict, false, 2, 3), "write conflict contract fixture");
    MIB_REQUIRE(writeContractFixture(unknown, false, 9), "write unknown contract fixture");
    checkContract(missing, true, 1);
    checkContract(partial, true, 2);
    checkContract(malformed, false, 0);
    checkContract(conflict, false, 0);
    checkContract(unknown, false, 0);
    return mib::test::exitCode();
}
