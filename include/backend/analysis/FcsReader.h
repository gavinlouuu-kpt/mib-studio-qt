#pragma once

#include "backend/analysis/ColumnarEvaluation.h"
#include "backend/analysis/SourceAnalysis.h"

#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// FCS 3.0 / 3.1 list-mode import feeding the native evaluator (F15 read
// side, slice F-2). Native C++ only: no Python, no vendor library.
//
// Supported: HEADER (segment offsets, $BEGINDATA/$ENDDATA when the header
// offsets are 0, supplemental TEXT), TEXT with any delimiter and doubled-
// delimiter escapes, case-insensitive keywords, $MODE L, $DATATYPE F (32-bit),
// D (64-bit) and I (8/16/32/64-bit, per-parameter $PnB), $BYTEORD ascending
// (little-endian) or descending (big-endian), $PnR range masking for
// integers, $PnE log decoding for integers, optional linear $PnG division.
//
// Refused (FcsError, nothing read): $MODE C/U (histograms), $DATATYPE A,
// integer $PnB that is not 8/16/32/64 (packed bits), mixed byte orders
// (e.g. 3,4,1,2), F/D with $PnB other than 32/64, DATA outside the file or
// shorter than $TOT x event width, missing required keywords, duplicate
// keywords, malformed TEXT.
//
// Present but not applied (reported in `unsupported`, never silent):
// compensation ($SPILLOVER/$SPILL/$COMP), $PnCALIBRATION, $PnD display
// scales, $TIMESTEP time scaling, $PnE log flags on F/D data, additional
// data sets ($NEXTDATA), the ANALYSIS segment, trailing DATA bytes, and
// $PnG when gain is not applied.
namespace backend::analysis {

struct FcsError : std::invalid_argument {
    using std::invalid_argument::invalid_argument;
};

struct FcsParameter {
    std::size_t index{0};    // 1-based $Pn index
    std::string id;          // analysis parameter id ($PnN, made graph-safe and unique)
    std::string name;        // $PnN as stored
    std::string label;       // $PnS (may be empty)
    unsigned bits{0};        // $PnB
    double range{0};         // $PnR
    double logDecades{0};    // $PnE f1
    double logOffset{0};     // $PnE f2 (0 with f1 > 0 is read as 1, reported)
    double gain{1};          // $PnG (1 when absent)
    bool hasGain{false};
    std::uint64_t mask{0};   // integer range mask (0: none)
    std::string scale;       // "linear", "log-decoded", "linear / $PnG"
    std::string unit;        // explicit unit string of the analysis column
};

struct FcsIssue {
    std::string keyword;
    std::string message;
};

struct FcsOptions {
    // Divide linear values by $PnG. Off by default: the stored channel value
    // is what FlowJo and the reference gate use; the choice is recorded.
    bool applyGain{false};
};

struct FcsFile {
    std::string path;
    std::string version; // "FCS3.0" or "FCS3.1"
    std::uint64_t textBegin{0}, textEnd{0}, dataBegin{0}, dataEnd{0};
    std::uint64_t analysisBegin{0}, analysisEnd{0};
    char delimiter{'/'};
    char dataType{'F'};
    bool bigEndian{false};
    std::uint64_t events{0};
    std::size_t eventBytes{0};
    std::uint64_t fileBytes{0};
    std::map<std::string, std::string> keywords; // keys uppercased
    std::vector<FcsParameter> parameters;
    std::vector<FcsIssue> unsupported;
    FcsOptions options;
};

// The keyword/value pairs of one TEXT segment (first byte = delimiter;
// doubled delimiters are literal). Keys are uppercased. Throws FcsError.
std::map<std::string, std::string> parseFcsText(std::string_view segment);

// HEADER + TEXT (+ supplemental TEXT) of `path`, validated for list-mode
// reading. Reads no DATA. Throws FcsError naming the refusal.
FcsFile readFcsMetadata(const std::string& path, const FcsOptions& options = {});

// The DATA segment as chunked analysis columns (one segment "events"),
// values per FcsParameter::scale; nonfinite values are missing. Each scan
// opens the file, reads at most chunkEvents events at a time and closes it.
class FcsColumns final : public EventColumns {
public:
    explicit FcsColumns(FcsFile file);
    const std::vector<EventSegment>& segments() const override { return segments_; }
    const std::map<std::string, std::string>& parameters() const override { return units_; }
    bool scan(const std::vector<std::string>& columns, std::size_t chunkEvents,
              const ChunkSink& sink) const override;
    const FcsFile& file() const { return file_; }

private:
    FcsFile file_;
    std::vector<EventSegment> segments_;
    std::map<std::string, std::string> units_;
};

// Host glue (Studio and Review hosts, acceptance tools):
// {"kind":"fcs","path":P[,"applyGain":bool]} -> the F-source/2 handle of the
// file (scope "fcs", full-file SHA-256 identity, explicit units, an "import"
// block with every parameter's keywords and the unsupported constructs).
nlohmann::json fcsSourceDescriptor(const nlohmann::json& descriptor);
// Lends FcsColumns for an F-source/2 handle of scope "fcs" after re-verifying
// its full-file SHA-256 (cached by path, size and write time). Throws.
void lendFcsColumns(const nlohmann::json& source, const ColumnVisitor& use);
} // namespace backend::analysis
