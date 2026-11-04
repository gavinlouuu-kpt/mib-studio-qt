#pragma once
// Test-only FCS 3.x writer (F-2): independent of FcsReader. Builds HEADER,
// TEXT (doubling the delimiter inside keywords and values) and DATA bytes
// from explicit per-parameter encodings, so tests can check the reader
// against values they chose.
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace mib::test::fcs {

struct Column {
    std::string name;  // $PnN
    std::string label; // $PnS ("" = absent)
    unsigned bits{32};
    std::string range{"262144"};
    std::string amplification{"0,0"};
    std::string gain; // "" = absent
};

struct Spec {
    std::string version{"FCS3.1"};
    char delimiter{'/'};
    char dataType{'F'};
    bool bigEndian{true};
    std::string byteOrder; // "" = derived from bigEndian and width
    bool offsetsInText{false}; // HEADER DATA offsets 0, $BEGINDATA/$ENDDATA only
    std::string mode{"L"};
    std::vector<Column> columns;
    std::vector<std::pair<std::string, std::string>> extra; // more keywords
    bool lowercaseKeys{false};
    std::int64_t totOverride{-1};
};

inline void put(std::vector<unsigned char>& out, std::uint64_t value, unsigned bytes, bool bigEndian) {
    for (unsigned k = 0; k < bytes; ++k) {
        const unsigned shift = 8 * (bigEndian ? bytes - 1 - k : k);
        out.push_back(static_cast<unsigned char>((value >> shift) & 0xFF));
    }
}
// One event's stored value of a column (float/double bit patterns or an
// unsigned integer), appended in the spec's byte order.
inline void putValue(std::vector<unsigned char>& out, const Spec& s, const Column& c, double value,
                     std::uint64_t integer) {
    if (s.dataType == 'F') {
        const float f = static_cast<float>(value);
        std::uint32_t bits;
        std::memcpy(&bits, &f, 4);
        put(out, bits, 4, s.bigEndian);
    } else if (s.dataType == 'D') {
        std::uint64_t bits;
        std::memcpy(&bits, &value, 8);
        put(out, bits, 8, s.bigEndian);
    } else {
        put(out, integer, c.bits / 8, s.bigEndian);
    }
}

inline std::string escape(const std::string& text, char delimiter) {
    std::string out;
    for (const char c : text) {
        out += c;
        if (c == delimiter) out += c;
    }
    return out;
}

// Writes the file; `data` is the DATA segment as the test encoded it.
inline bool write(const std::string& path, const Spec& s, std::uint64_t events,
                  const std::vector<unsigned char>& data) {
    std::string byteOrder = s.byteOrder;
    if (byteOrder.empty()) {
        const unsigned width = s.dataType == 'D' ? 8 : 4;
        for (unsigned k = 0; k < width; ++k) {
            if (k) byteOrder += ',';
            byteOrder += std::to_string(s.bigEndian ? width - k : k + 1);
        }
    }
    const auto key = [&](std::string k) {
        if (s.lowercaseKeys)
            for (auto& ch : k) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return k;
    };
    std::uint64_t textBegin = 58, dataBegin = 0, dataEnd = 0;
    std::string text;
    for (int pass = 0; pass < 4; ++pass) {
        std::vector<std::pair<std::string, std::string>> kv{
            {"$BEGINANALYSIS", "0"}, {"$ENDANALYSIS", "0"}, {"$BEGINSTEXT", "0"}, {"$ENDSTEXT", "0"},
            {"$BEGINDATA", std::to_string(dataBegin)}, {"$ENDDATA", std::to_string(dataEnd)},
            {"$BYTEORD", byteOrder}, {"$DATATYPE", std::string(1, s.dataType)}, {"$MODE", s.mode},
            {"$NEXTDATA", "0"}, {"$PAR", std::to_string(s.columns.size())},
            {"$TOT", std::to_string(s.totOverride >= 0 ? static_cast<std::uint64_t>(s.totOverride) : events)}};
        for (std::size_t i = 0; i < s.columns.size(); ++i) {
            const auto& c = s.columns[i];
            const auto p = "$P" + std::to_string(i + 1);
            kv.push_back({p + "N", c.name});
            if (!c.label.empty()) kv.push_back({p + "S", c.label});
            kv.push_back({p + "B", std::to_string(c.bits)});
            kv.push_back({p + "R", c.range});
            kv.push_back({p + "E", c.amplification});
            if (!c.gain.empty()) kv.push_back({p + "G", c.gain});
        }
        for (const auto& e : s.extra) kv.push_back(e);
        text.assign(1, s.delimiter);
        for (const auto& [k, v] : kv) {
            text += escape(key(k), s.delimiter);
            text += s.delimiter;
            text += escape(v, s.delimiter);
            text += s.delimiter;
        }
        const std::uint64_t newBegin = textBegin + text.size();
        const std::uint64_t newEnd = data.empty() ? newBegin : newBegin + data.size() - 1;
        if (newBegin == dataBegin && newEnd == dataEnd) break;
        dataBegin = newBegin;
        dataEnd = newEnd;
    }
    char header[59];
    const std::uint64_t hb = s.offsetsInText ? 0 : dataBegin, he = s.offsetsInText ? 0 : dataEnd;
    std::snprintf(header, sizeof(header), "%-6s    %8llu%8llu%8llu%8llu%8llu%8llu", s.version.c_str(),
                  static_cast<unsigned long long>(textBegin),
                  static_cast<unsigned long long>(textBegin + text.size() - 1),
                  static_cast<unsigned long long>(hb), static_cast<unsigned long long>(he), 0ULL, 0ULL);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(header, 58);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}
} // namespace mib::test::fcs
