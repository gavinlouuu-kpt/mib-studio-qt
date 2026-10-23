#include "backend/analysis/SourceAnalysis.h"
#include <cmath>
#include <set>
namespace backend::analysis {
using Json = nlohmann::json;
Json prepareAnalysisSource(const Json& source, const Json& strategy) {
    if (source.at("schema") != "F-source/1" || source.at("rows").size() > 10000)
        throw std::invalid_argument("Unsupported analysis source");
    auto result = source;
    const auto stages = strategy.value("transforms", Json::array());
    if (!stages.is_array() || stages.size() > 8)
        throw std::invalid_argument("Expected at most eight ordered transforms");
    for (const auto& stage : stages) {
        const std::set<std::string> names{"input", "inputUnit", "output",
                                          "kind",  "scale",     "offset"};
        if (!stage.is_object() || stage.size() != names.size())
            throw std::invalid_argument("Incomplete transform fields");
        for (auto i = stage.begin(); i != stage.end(); ++i)
            if (!names.count(i.key())) throw std::invalid_argument("Unknown transform field");
        const auto input = stage.at("input").get<std::string>(),
                   output = stage.at("output").get<std::string>(),
                   kind = stage.at("kind").get<std::string>();
        auto& units = result.at("parameters");
        if (!units.contains(input) || units.at(input) != stage.at("inputUnit"))
            throw std::invalid_argument("Transform input/unit unavailable or mismatched");
        if (output.empty() || output.size() > 64 ||
            output.find_first_not_of(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") !=
                std::string::npos ||
            units.contains(output))
            throw std::invalid_argument("Transform output must be a new unique parameter");
        const double scale = stage.at("scale").get<double>(),
                     offset = stage.at("offset").get<double>();
        if (!std::isfinite(scale) || scale <= 0 || !std::isfinite(offset) ||
            (kind != "linear" && kind != "asinh" && kind != "log10"))
            throw std::invalid_argument("Unsupported transform or invalid scale/offset");
        units[output] = "dimensionless";
        for (auto& row : result.at("rows")) {
            auto& values = row.at("measurements");
            Json transformed = nullptr;
            if (values.contains(input) && values.at(input).is_number()) {
                const double raw = values.at(input).get<double>();
                if (std::isfinite(raw)) {
                    long double value = (static_cast<long double>(raw) - offset) / scale;
                    if (kind == "asinh") value = std::asinh(value);
                    if (kind == "log10")
                        value = value > 0 ? std::log10(value)
                                          : std::numeric_limits<long double>::quiet_NaN();
                    const double number = static_cast<double>(value);
                    if (std::isfinite(number)) transformed = number;
                }
            }
            values[output] = transformed;
        }
    }
    result["transforms"] = stages;
    result["parameterFingerprint"] =
        contentHash(Json{{"rawParameters", source.at("parameters")}, {"transforms", stages}});
    return result;
}
} // namespace backend::analysis
