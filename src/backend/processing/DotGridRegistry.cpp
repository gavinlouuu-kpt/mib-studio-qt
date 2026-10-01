#include "backend/processing/DotGridRegistry.h"

#include <fstream>
#include <sstream>

#if __has_include(<nlohmann/json.hpp>)
#include <nlohmann/json.hpp>
#define MIB_DOTGRID_REGISTRY_HAVE_JSON 1
#else
#define MIB_DOTGRID_REGISTRY_HAVE_JSON 0
#endif

namespace backend::dotgrid {

namespace {

std::string designKey(const Design& d) {
    const auto& p = d.codebook->params();
    std::ostringstream os;
    os.precision(17);
    os << d.id << ':' << p.seed << ':' << p.columns << 'x' << p.rows << ':' << p.pitchUm << '/'
       << p.dotDiameterUm << '/' << p.displacementUm << '@' << p.originXUm << ',' << p.originYUm
       << ':' << d.codebook->chips().size();
    return os.str();
}

} // namespace

bool Registry::validId(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (size_t k = 0; k < id.size(); ++k) {
        const char c = id[k];
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (k == 0 ? !alnum : !(alnum || c == '.' || c == '_' || c == '-')) return false;
    }
    return true;
}

bool Registry::add(Design design, std::string* errorOut) {
    auto fail = [&](const std::string& msg) {
        if (errorOut) *errorOut = msg;
        return false;
    };
    if (!design.codebook || !design.codebook->valid())
        return fail("design '" + design.id + "' has no codebook");
    for (const auto& d : designs_) {
        if (d.id == design.id) return fail("duplicate design id '" + design.id + "'");
        if (d.codebook->params().seed == design.codebook->params().seed)
            return fail("seed " + std::to_string(design.codebook->params().seed) + " used by both '" +
                        d.id + "' and '" + design.id + "'");
    }
    designs_.push_back(std::move(design));
    return true;
}

void Registry::merge(const Registry& other, std::vector<std::string>* warnings) {
    for (const auto& d : other.designs_) {
        bool duplicate = false;
        for (const auto& mine : designs_)
            if (designKey(mine) == designKey(d)) duplicate = true;
        if (duplicate) continue;
        std::string err;
        if (!add(d, &err) && warnings) warnings->push_back("skipped '" + d.id + "': " + err);
    }
}

const Design* Registry::find(const std::string& id) const {
    for (const auto& d : designs_)
        if (d.id == id) return &d;
    return nullptr;
}

std::string Registry::fingerprint() const {
    std::string out;
    for (const auto& d : designs_) {
        out += designKey(d);
        out += ';';
    }
    return out;
}

Registry Registry::single(std::shared_ptr<const Codebook> codebook, std::string id,
                          std::string name) {
    Registry r;
    Design d;
    d.id = std::move(id);
    d.name = name.empty() ? (codebook ? codebook->designName() : std::string{}) : std::move(name);
    d.status = "active";
    d.codebook = std::move(codebook);
    if (d.codebook) r.designs_.push_back(std::move(d));
    return r;
}

bool Registry::parse(const std::string& jsonText, Registry& out, std::string* errorOut) {
#if MIB_DOTGRID_REGISTRY_HAVE_JSON
    std::string context;
    try {
        const nlohmann::json j = nlohmann::json::parse(jsonText);
        if (j.value("version", 0) != kRegistryVersion) {
            if (errorOut) *errorOut = "unsupported registry version";
            return false;
        }
        Registry reg;
        for (const auto& e : j.at("designs")) {
            Design d;
            d.id = e.at("id").get<std::string>();
            context = "design '" + d.id + "': ";
            if (!validId(d.id)) {
                if (errorOut) *errorOut = context + "id must be a lowercase slug ([a-z0-9._-])";
                return false;
            }
            d.name = e.value("name", d.id);
            d.revision = e.value("revision", std::string{});
            d.status = e.value("status", std::string{"active"});
            if (d.status != "active" && d.status != "retired") {
                if (errorOut) *errorOut = context + "status must be active or retired";
                return false;
            }
            CodebookParams p;
            p.seed = e.at("seed").get<uint64_t>();
            p.columns = e.at("columns").get<int>();
            p.rows = e.at("rows").get<int>();
            p.pitchUm = e.at("pitch_um").get<double>();
            p.dotDiameterUm = e.at("dot_diameter_um").get<double>();
            p.displacementUm = e.at("displacement_um").get<double>();
            if (e.contains("origin_um")) {
                p.originXUm = e.at("origin_um").at(0).get<double>();
                p.originYUm = e.at("origin_um").at(1).get<double>();
            }
            std::vector<Chip> chips;
            for (const auto& c : e.value("chips", nlohmann::json::array())) {
                Chip chip;
                chip.name = c.at("name").get<std::string>();
                chip.xMinUm = c.at("x_min_um").get<double>();
                chip.yMinUm = c.at("y_min_um").get<double>();
                chip.xMaxUm = c.at("x_max_um").get<double>();
                chip.yMaxUm = c.at("y_max_um").get<double>();
                chips.push_back(std::move(chip));
            }
            d.codebook =
                std::make_shared<const Codebook>(Codebook::generate(p, std::move(chips), d.id));
            std::string err;
            if (!reg.add(std::move(d), &err)) {
                if (errorOut) *errorOut = err;
                return false;
            }
        }
        out = std::move(reg);
        return true;
    } catch (const std::exception& e) {
        if (errorOut) *errorOut = context + e.what();
        return false;
    }
#else
    (void)jsonText;
    (void)out;
    if (errorOut) *errorOut = "JSON support not compiled in";
    return false;
#endif
}

bool Registry::loadFile(const std::string& path, Registry& out, std::string* errorOut) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (errorOut) *errorOut = "cannot open " + path;
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return parse(ss.str(), out, errorOut);
}

} // namespace backend::dotgrid
