// FCS 3.0 / 3.1 list-mode reader (F15 read side, slice F-2). See FcsReader.h.
#include "backend/analysis/FcsReader.h"

#include "backend/processing/ProcessingCoreSha256.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <set>

namespace backend::analysis {
namespace {
using Json = nlohmann::json;
constexpr double kMissing = std::numeric_limits<double>::quiet_NaN();
constexpr std::size_t kMaxTextBytes = 64ULL * 1024 * 1024;
constexpr std::size_t kMaxParameters = 1024;
constexpr std::size_t kReadBytes = 4ULL * 1024 * 1024; // one physical DATA read

[[noreturn]] void fail(const std::string& message) { throw FcsError("FCS: " + message); }

std::string trim(std::string_view text) {
    std::size_t a = 0, b = text.size();
    while (a < b && (std::isspace(static_cast<unsigned char>(text[a])) || text[a] == '\0')) ++a;
    while (b > a && (std::isspace(static_cast<unsigned char>(text[b - 1])) || text[b - 1] == '\0')) --b;
    return std::string(text.substr(a, b - a));
}
std::string upper(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}
std::uint64_t unsignedValue(const std::string& text, const std::string& what) {
    const auto t = trim(text);
    std::uint64_t value{};
    const auto parsed = std::from_chars(t.data(), t.data() + t.size(), value);
    if (t.empty() || parsed.ec != std::errc{} || parsed.ptr != t.data() + t.size())
        fail(what + " is not an unsigned integer: '" + text + "'");
    return value;
}
double numberValue(const std::string& text, const std::string& what) {
    const auto t = trim(text);
    try {
        std::size_t used = 0;
        const double v = std::stod(t, &used);
        if (used != t.size() || !std::isfinite(v)) fail(what + " is not a finite number: '" + text + "'");
        return v;
    } catch (const FcsError&) {
        throw;
    } catch (const std::exception&) {
        fail(what + " is not a number: '" + text + "'");
    }
}
std::string readRange(std::ifstream& in, std::uint64_t begin, std::uint64_t end, const std::string& what) {
    if (end < begin) fail(what + " segment ends before it begins");
    const auto size = end - begin + 1;
    if (size > kMaxTextBytes) fail(what + " segment exceeds 64 MiB");
    std::string data(static_cast<std::size_t>(size), '\0');
    in.clear();
    in.seekg(static_cast<std::streamoff>(begin));
    in.read(data.data(), static_cast<std::streamsize>(size));
    if (in.gcount() != static_cast<std::streamsize>(size)) fail(what + " segment lies outside the file");
    return data;
}
const std::string* keyword(const FcsFile& f, const std::string& key) {
    const auto it = f.keywords.find(key);
    return it == f.keywords.end() ? nullptr : &it->second;
}
const std::string& required(const FcsFile& f, const std::string& key) {
    const auto* v = keyword(f, key);
    if (!v) fail("required keyword " + key + " is missing");
    return *v;
}
std::string graphSafe(const std::string& name, std::size_t index) {
    std::string id;
    for (const char c : name)
        id += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : '_';
    if (id.size() > 56) id.resize(56);
    if (id.empty()) id = "P" + std::to_string(index);
    return id;
}
// Bytes of one value, as stored, into a uint64 (little-endian assembly).
std::uint64_t loadUnsigned(const unsigned char* p, unsigned bytes, bool bigEndian) {
    std::uint64_t v = 0;
    for (unsigned k = 0; k < bytes; ++k) {
        const unsigned shift = 8 * (bigEndian ? bytes - 1 - k : k);
        v |= static_cast<std::uint64_t>(p[k]) << shift;
    }
    return v;
}
} // namespace

std::map<std::string, std::string> parseFcsText(std::string_view segment) {
    if (segment.size() < 2) fail("TEXT segment is empty");
    const char delimiter = segment[0];
    if (delimiter == '\0') fail("TEXT delimiter is NUL");
    std::vector<std::string> tokens;
    std::size_t i = 1;
    while (i < segment.size()) {
        std::string token;
        bool closed = false;
        while (i < segment.size()) {
            const char c = segment[i];
            if (c == delimiter) {
                if (i + 1 < segment.size() && segment[i + 1] == delimiter) {
                    token += delimiter; // doubled delimiter: a literal delimiter
                    i += 2;
                    continue;
                }
                ++i;
                closed = true;
                break;
            }
            token += c;
            ++i;
        }
        // Padding after the final delimiter (spaces or NULs) is not a token.
        if (!closed && trim(token).empty()) break;
        tokens.push_back(std::move(token));
    }
    if (tokens.size() % 2 != 0) fail("TEXT has a keyword without a value (" + std::to_string(tokens.size()) + " tokens)");
    std::map<std::string, std::string> out;
    for (std::size_t k = 0; k < tokens.size(); k += 2) {
        const auto key = upper(trim(tokens[k]));
        if (key.empty()) fail("TEXT has an empty keyword");
        if (!out.emplace(key, tokens[k + 1]).second) fail("TEXT repeats keyword " + key);
    }
    return out;
}

FcsFile readFcsMetadata(const std::string& path, const FcsOptions& options) {
    FcsFile f;
    f.path = path;
    f.options = options;
    std::error_code ec;
    f.fileBytes = std::filesystem::file_size(path, ec);
    if (ec) fail("cannot read " + path);
    std::ifstream in(path, std::ios::binary);
    if (!in) fail("cannot open " + path);
    char header[58];
    in.read(header, sizeof(header));
    if (in.gcount() != static_cast<std::streamsize>(sizeof(header))) fail("file is shorter than the 58-byte HEADER");
    f.version.assign(header, 6);
    if (f.version != "FCS3.0" && f.version != "FCS3.1")
        fail("unsupported version '" + f.version + "' (FCS3.0 and FCS3.1 are read)");
    const auto field = [&](int offset) -> std::uint64_t {
        const auto text = trim(std::string_view(header + offset, 8));
        if (text.empty()) return 0;
        return unsignedValue(text, "HEADER offset");
    };
    f.textBegin = field(10);
    f.textEnd = field(18);
    f.dataBegin = field(26);
    f.dataEnd = field(34);
    f.analysisBegin = field(42);
    f.analysisEnd = field(50);
    if (f.textBegin < 58 || f.textEnd < f.textBegin || f.textEnd >= f.fileBytes)
        fail("TEXT segment offsets are invalid");
    const auto text = readRange(in, f.textBegin, f.textEnd, "TEXT");
    f.delimiter = text[0];
    f.keywords = parseFcsText(text);
    // Supplemental TEXT (3.0/3.1), same delimiter rules; no repeats allowed.
    const auto* stextBegin = keyword(f, "$BEGINSTEXT");
    const auto* stextEnd = keyword(f, "$ENDSTEXT");
    if (stextBegin && stextEnd) {
        const auto b = unsignedValue(*stextBegin, "$BEGINSTEXT"), e = unsignedValue(*stextEnd, "$ENDSTEXT");
        if (b != 0 || e != 0) {
            if (e >= f.fileBytes || e < b) fail("supplemental TEXT offsets are invalid");
            for (auto& [k, v] : parseFcsText(readRange(in, b, e, "supplemental TEXT")))
                if (!f.keywords.emplace(k, v).second) fail("supplemental TEXT repeats keyword " + k);
        }
    }
    const auto mode = upper(trim(required(f, "$MODE")));
    if (mode != "L") fail("$MODE " + mode + " is not list mode (histogram data sets are not read)");
    const auto type = upper(trim(required(f, "$DATATYPE")));
    if (type != "F" && type != "D" && type != "I")
        fail("$DATATYPE " + type + " is unsupported (F, D and I are read; ASCII A is not)");
    f.dataType = type[0];
    // $BYTEORD: 1,2,...,n little-endian; n,...,2,1 big-endian; else refused.
    {
        std::vector<std::uint64_t> order;
        std::string item;
        for (const char c : required(f, "$BYTEORD") + ",") {
            if (c == ',') {
                order.push_back(unsignedValue(item, "$BYTEORD"));
                item.clear();
            } else {
                item += c;
            }
        }
        bool ascending = true, descending = true;
        for (std::size_t k = 0; k < order.size(); ++k) {
            ascending = ascending && order[k] == k + 1;
            descending = descending && order[k] == order.size() - k;
        }
        if (order.empty() || (!ascending && !descending))
            fail("$BYTEORD " + required(f, "$BYTEORD") + " is a mixed byte order (only ascending or descending)");
        f.bigEndian = descending && order.size() > 1;
    }
    const auto par = unsignedValue(required(f, "$PAR"), "$PAR");
    if (par == 0 || par > kMaxParameters) fail("$PAR must be 1.." + std::to_string(kMaxParameters));
    f.events = unsignedValue(required(f, "$TOT"), "$TOT");
    if (const auto* next = keyword(f, "$NEXTDATA")) {
        if (unsignedValue(*next, "$NEXTDATA") != 0)
            f.unsupported.push_back({"$NEXTDATA", "additional data sets in this file are not read; only the first"});
    } else {
        f.unsupported.push_back({"$NEXTDATA", "required keyword absent; the file is read as one data set"});
    }
    // DATA offsets: the HEADER, or $BEGINDATA/$ENDDATA when it records 0
    // (files above 99,999,999 bytes).
    const auto* beginData = keyword(f, "$BEGINDATA");
    const auto* endData = keyword(f, "$ENDDATA");
    if (f.dataBegin == 0 && f.dataEnd == 0) {
        if (!beginData || !endData) fail("DATA offsets are 0 in the HEADER and $BEGINDATA/$ENDDATA are absent");
        f.dataBegin = unsignedValue(*beginData, "$BEGINDATA");
        f.dataEnd = unsignedValue(*endData, "$ENDDATA");
    } else if (beginData && endData &&
               (unsignedValue(*beginData, "$BEGINDATA") != f.dataBegin ||
                unsignedValue(*endData, "$ENDDATA") != f.dataEnd)) {
        f.unsupported.push_back({"$BEGINDATA", "disagrees with the HEADER DATA offsets; the HEADER is used"});
    }
    if (f.analysisEnd > f.analysisBegin)
        f.unsupported.push_back({"ANALYSIS", "the ANALYSIS segment is present and not read"});
    std::set<std::string> ids;
    for (std::size_t n = 1; n <= par; ++n) {
        const auto key = [&](const char* suffix) { return "$P" + std::to_string(n) + suffix; };
        FcsParameter p;
        p.index = n;
        p.name = trim(required(f, key("N")));
        if (const auto* s = keyword(f, key("S"))) p.label = trim(*s);
        const auto bits = trim(required(f, key("B")));
        if (bits == "*") fail(key("B") + " '*' (ASCII data) is unsupported");
        p.bits = static_cast<unsigned>(unsignedValue(bits, key("B")));
        p.range = numberValue(required(f, key("R")), key("R"));
        if (const auto* e = keyword(f, key("E"))) {
            const auto comma = e->find(',');
            if (comma == std::string::npos) fail(key("E") + " must be f1,f2");
            p.logDecades = numberValue(e->substr(0, comma), key("E"));
            p.logOffset = numberValue(e->substr(comma + 1), key("E"));
        } else {
            f.unsupported.push_back({key("E"), "required keyword absent; read as linear (0,0)"});
        }
        if (const auto* g = keyword(f, key("G"))) {
            p.gain = numberValue(*g, key("G"));
            p.hasGain = true;
            if (p.gain <= 0) fail(key("G") + " must be positive");
        }
        if (f.dataType == 'F' && p.bits != 32) fail("$DATATYPE F requires " + key("B") + " 32");
        if (f.dataType == 'D' && p.bits != 64) fail("$DATATYPE D requires " + key("B") + " 64");
        if (f.dataType == 'I' && p.bits != 8 && p.bits != 16 && p.bits != 32 && p.bits != 64)
            fail(key("B") + " " + std::to_string(p.bits) + " is packed integer data (8/16/32/64 are read)");
        p.scale = "linear";
        if (f.dataType == 'I') {
            if (p.range > 0 && p.range < std::ldexp(1.0, static_cast<int>(p.bits))) {
                std::uint64_t pow2 = 1;
                while (static_cast<double>(pow2) < p.range) pow2 <<= 1;
                p.mask = pow2 - 1;
            }
            if (p.logDecades > 0) {
                if (p.logOffset == 0) {
                    p.logOffset = 1;
                    f.unsupported.push_back({key("E"), "f2 = 0 with f1 > 0 is invalid; read as f2 = 1"});
                }
                if (!(p.range > 0)) fail(key("R") + " must be positive for log decoding");
                p.scale = "log-decoded";
            }
        } else if (p.logDecades != 0 || p.logOffset != 0) {
            f.unsupported.push_back({key("E"), "log amplification declared for floating-point data; not applied"});
        }
        if (p.scale == "linear" && p.hasGain && p.gain != 1) {
            if (options.applyGain)
                p.scale = "linear / $PnG";
            else
                f.unsupported.push_back({key("G"), "gain recorded and not applied (FcsOptions.applyGain false)"});
        }
        p.unit = p.scale == "log-decoded"   ? "a.u. (FCS log-decoded scale value)"
                 : p.scale == "linear"      ? "a.u. (FCS channel value)"
                                            : "a.u. (FCS channel value / $PnG)";
        auto id = graphSafe(p.name, n);
        if (!ids.insert(id).second) {
            id += "_" + std::to_string(n);
            if (!ids.insert(id).second) fail("parameter names are not unique after normalization");
        }
        p.id = id;
        if (keyword(f, key("CALIBRATION")))
            f.unsupported.push_back({key("CALIBRATION"), "calibration to physical units is not applied"});
        if (keyword(f, key("D")))
            f.unsupported.push_back({key("D"), "display scale hint is not applied (values are not transformed)"});
        f.eventBytes += p.bits / 8;
        f.parameters.push_back(std::move(p));
    }
    for (const char* comp : {"$SPILLOVER", "$SPILL", "SPILL", "SPILLOVER", "$COMP", "$DFC1TO2"})
        if (keyword(f, comp))
            f.unsupported.push_back({comp, "compensation is present and not applied (F03)"});
    if (keyword(f, "$TIMESTEP"))
        f.unsupported.push_back({"$TIMESTEP", "time is reported in stored units; $TIMESTEP is not applied"});
    const std::uint64_t needed = f.events * f.eventBytes;
    if (f.events != 0 && needed / f.events != f.eventBytes) fail("$TOT x event width overflows");
    if (f.events == 0) return f;
    if (f.dataEnd < f.dataBegin || f.dataEnd >= f.fileBytes) fail("DATA segment lies outside the file");
    const auto available = f.dataEnd - f.dataBegin + 1;
    if (available < needed)
        fail("DATA holds " + std::to_string(available) + " bytes; $TOT x event width needs " + std::to_string(needed));
    if (available > needed)
        f.unsupported.push_back({"DATA", std::to_string(available - needed) + " trailing DATA bytes ignored"});
    return f;
}

FcsColumns::FcsColumns(FcsFile file) : file_(std::move(file)) {
    segments_.push_back({"events", file_.events});
    for (const auto& p : file_.parameters) units_.emplace(p.id, p.unit);
}

bool FcsColumns::scan(const std::vector<std::string>& columns, std::size_t chunkEvents,
                      const ChunkSink& sink) const {
    if (chunkEvents == 0) throw std::invalid_argument("Chunk size must be positive");
    std::vector<const FcsParameter*> wanted;
    std::vector<std::size_t> offsets;
    for (const auto& id : columns) {
        std::size_t offset = 0;
        const FcsParameter* found = nullptr;
        for (const auto& p : file_.parameters) {
            if (p.id == id) {
                found = &p;
                break;
            }
            offset += p.bits / 8;
        }
        if (!found) throw std::invalid_argument("Column unavailable in FCS file: " + id);
        wanted.push_back(found);
        offsets.push_back(offset);
    }
    if (file_.events == 0) return true;
    std::ifstream in(file_.path, std::ios::binary);
    if (!in) throw FcsError("FCS: cannot reopen " + file_.path);
    const std::size_t perRead =
        std::max<std::size_t>(1, std::min<std::size_t>(chunkEvents, kReadBytes / std::max<std::size_t>(1, file_.eventBytes)));
    std::vector<unsigned char> bytes;
    std::vector<std::vector<double>> buffers(columns.size());
    std::vector<const double*> pointers(columns.size());
    for (std::uint64_t begin = 0; begin < file_.events;) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(perRead, file_.events - begin));
        bytes.resize(count * file_.eventBytes);
        in.seekg(static_cast<std::streamoff>(file_.dataBegin + begin * file_.eventBytes));
        in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (in.gcount() != static_cast<std::streamsize>(bytes.size()))
            throw FcsError("FCS: DATA ended early at event " + std::to_string(begin));
        for (std::size_t c = 0; c < wanted.size(); ++c) {
            const auto& p = *wanted[c];
            auto& out = buffers[c];
            out.resize(count);
            const unsigned width = p.bits / 8;
            for (std::size_t j = 0; j < count; ++j) {
                const unsigned char* at = bytes.data() + j * file_.eventBytes + offsets[c];
                double v;
                if (file_.dataType == 'F') {
                    const auto raw = static_cast<std::uint32_t>(loadUnsigned(at, 4, file_.bigEndian));
                    float x;
                    std::memcpy(&x, &raw, 4);
                    v = x;
                } else if (file_.dataType == 'D') {
                    const auto raw = loadUnsigned(at, 8, file_.bigEndian);
                    std::memcpy(&v, &raw, 8);
                } else {
                    auto raw = loadUnsigned(at, width, file_.bigEndian);
                    if (p.mask) raw &= p.mask;
                    v = static_cast<double>(raw);
                    if (p.scale == "log-decoded")
                        v = p.logOffset * std::pow(10.0, p.logDecades * v / p.range);
                }
                if (p.scale == "linear / $PnG") v /= p.gain;
                out[j] = std::isfinite(v) ? v : kMissing;
            }
            pointers[c] = out.data();
        }
        if (!sink(begin, count, pointers)) return false;
        begin += count;
    }
    return true;
}

namespace {
struct HashEntry {
    std::string path;
    std::uintmax_t size{0};
    std::filesystem::file_time_type time{};
    std::string hash;
};
std::mutex hashMutex;
std::vector<HashEntry> hashCache;

std::string fileFingerprint(const std::string& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) fail("cannot read " + path);
    const auto time = std::filesystem::last_write_time(path, ec);
    if (ec) fail("cannot read " + path);
    {
        std::scoped_lock lock(hashMutex);
        for (const auto& e : hashCache)
            if (e.path == path && e.size == size && e.time == time) return e.hash;
    }
    std::string error;
    const auto hash = processing::fileSha256(path, &error, {});
    if (hash.empty()) fail("fingerprint failed: " + error);
    if (std::filesystem::file_size(path, ec) != size || std::filesystem::last_write_time(path, ec) != time)
        fail("file changed while fingerprinting: " + path);
    std::scoped_lock lock(hashMutex);
    hashCache.insert(hashCache.begin(), {path, size, time, "sha256:" + hash});
    if (hashCache.size() > 16) hashCache.pop_back();
    return hashCache.front().hash;
}

std::string byteOrderName(const FcsFile& f) { return f.bigEndian ? "big-endian" : "little-endian"; }
} // namespace

Json fcsSourceDescriptor(const Json& descriptor) {
    if (!descriptor.is_object() || descriptor.value("kind", std::string{}) != "fcs" || !descriptor.contains("path") ||
        !descriptor.at("path").is_string() || descriptor.size() > 3 ||
        (descriptor.size() == 3 && (!descriptor.contains("applyGain") || !descriptor.at("applyGain").is_boolean())))
        throw std::invalid_argument("Expected {kind: fcs, path[, applyGain]}");
    FcsOptions options;
    options.applyGain = descriptor.value("applyGain", false);
    const auto path = descriptor.at("path").get<std::string>();
    const auto file = readFcsMetadata(path, options);
    const auto hash = fileFingerprint(path);
    Json parameters = Json::object(), imported = Json::array(), issues = Json::array(), keywords = Json::object();
    for (const auto& p : file.parameters) {
        parameters[p.id] = p.unit;
        imported.push_back({{"id", p.id},
                            {"index", p.index},
                            {"name", p.name},
                            {"label", p.label},
                            {"bits", p.bits},
                            {"range", p.range},
                            {"amplification", Json::array({p.logDecades, p.logOffset})},
                            {"gain", p.hasGain ? Json(p.gain) : Json(nullptr)},
                            {"scale", p.scale},
                            {"unit", p.unit}});
    }
    for (const auto& i : file.unsupported) issues.push_back({{"keyword", i.keyword}, {"message", i.message}});
    for (const auto& [k, v] : file.keywords) {
        if (k.size() > 2 && k[0] == '$' && k[1] == 'P' && std::isdigit(static_cast<unsigned char>(k[2]))) continue;
        if (keywords.size() >= 64) break;
        keywords[k] = v.size() > 256 ? v.substr(0, 256) : v;
    }
    const auto fingerprint = options.applyGain ? contentHash(Json{{"file", hash}, {"applyGain", true}}) : hash;
    return Json{{"schema", kColumnarSourceSchema},
                {"storage", "columnar"},
                {"sourceKey", fingerprint},
                {"fingerprint", fingerprint},
                {"fingerprintKind", options.applyGain ? "full-file-sha256+applied-gain" : "full-file-sha256"},
                {"fileFingerprint", hash},
                {"scope", "fcs"},
                {"path", path},
                {"parameters", parameters},
                {"segments", Json::array({Json{{"set", "events"}, {"count", std::to_string(file.events)}}})},
                {"events", std::to_string(file.events)},
                {"populationComplete", true},
                {"finalized", true},
                {"retention",
                 "file-pinned: DATA is re-read from the file and its full-file SHA-256 re-verified on every "
                 "evaluation; no rows are copied"},
                {"import",
                 {{"format", file.version},
                  {"dataType", std::string(1, file.dataType)},
                  {"byteOrder", byteOrderName(file)},
                  {"applyGain", options.applyGain},
                  {"parameters", imported},
                  {"unsupported", issues},
                  {"keywords", keywords},
                  {"reader", "native FCS 3.0/3.1 list-mode reader (FcsReader.cpp)"}}}};
}

void lendFcsColumns(const Json& source, const ColumnVisitor& use) {
    if (!isColumnarSource(source) || source.value("scope", std::string{}) != "fcs")
        throw std::invalid_argument("Not an FCS columnar source");
    const auto path = source.at("path").get<std::string>();
    FcsOptions options;
    options.applyGain = source.at("import").at("applyGain").get<bool>();
    if (fileFingerprint(path) != source.at("fileFingerprint").get<std::string>())
        throw std::invalid_argument("Pinned FCS file no longer matches its fingerprint: " + path);
    FcsColumns columns(readFcsMetadata(path, options));
    for (const auto& p : columns.file().parameters)
        if (!source.at("parameters").contains(p.id) || source.at("parameters").at(p.id).get<std::string>() != p.unit)
            throw std::invalid_argument("FCS parameters changed since the source was resolved");
    use(columns);
}
} // namespace backend::analysis
