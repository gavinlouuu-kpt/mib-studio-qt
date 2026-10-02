#include "backend/processing/DotGridRegistry.h"

#include "backend/processing/DotGridCodec.h"

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
    os << d.id << ":c" << d.codecContract << ':' << p.seed << ':' << p.columns << 'x' << p.rows
       << ':' << p.pitchUm << '/' << p.dotDiameterUm << '/' << p.displacementUm << '@'
       << p.originXUm << ',' << p.originYUm << ':' << d.codebook->chips().size();
    return os.str();
}

std::string unsupportedKey(const UnsupportedDesign& d) {
    return d.id + ":c" + std::to_string(d.codecContract) + ':' + std::to_string(d.seed) + ":unsupported";
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

bool Registry::idOrSeedTaken(const std::string& id, uint64_t seed, std::string* errorOut) const {
    auto taken = [&](const std::string& other, uint64_t otherSeed) {
        if (other == id) {
            if (errorOut) *errorOut = "duplicate design id '" + id + "'";
            return true;
        }
        if (otherSeed == seed) {
            if (errorOut)
                *errorOut = "seed " + std::to_string(seed) + " used by both '" + other + "' and '" + id + "'";
            return true;
        }
        return false;
    };
    for (const auto& d : designs_)
        if (taken(d.id, d.codebook->params().seed)) return true;
    for (const auto& u : unsupported_)
        if (taken(u.id, u.seed)) return true;
    return false;
}

bool Registry::add(Design design, std::string* errorOut) {
    if (!design.codebook || !design.codebook->valid()) {
        if (errorOut) *errorOut = "design '" + design.id + "' has no codebook";
        return false;
    }
    if (idOrSeedTaken(design.id, design.codebook->params().seed, errorOut)) return false;
    designs_.push_back(std::move(design));
    return true;
}

bool Registry::addUnsupported(UnsupportedDesign design, std::string* errorOut) {
    if (idOrSeedTaken(design.id, design.seed, errorOut)) return false;
    unsupported_.push_back(std::move(design));
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
    for (const auto& u : other.unsupported_) {
        bool duplicate = false;
        for (const auto& mine : unsupported_)
            if (unsupportedKey(mine) == unsupportedKey(u)) duplicate = true;
        if (duplicate) continue;
        std::string err;
        if (!addUnsupported(u, &err) && warnings) warnings->push_back("skipped '" + u.id + "': " + err);
    }
}

Registry Registry::withContract(int contract) const {
    Registry r;
    for (const auto& d : designs_)
        if (d.codecContract == contract) r.designs_.push_back(d);
    return r;
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
    for (const auto& u : unsupported_) {
        out += unsupportedKey(u);
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
    d.codecContract = kCodecContract1;
    d.codebook = std::move(codebook);
    if (d.codebook) r.designs_.push_back(std::move(d));
    return r;
}

bool Registry::parse(const std::string& jsonText, Registry& out, std::string* errorOut,
                     const CodecSet* codecs) {
#if MIB_DOTGRID_REGISTRY_HAVE_JSON
    const CodecSet bundled = codecs ? CodecSet{} : CodecSet::bundled();
    const CodecSet& active = codecs ? *codecs : bundled;
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
            if (!e.contains("codec_contract") || !e.at("codec_contract").is_number_integer() ||
                e.at("codec_contract").get<int>() < 1) {
                if (errorOut) *errorOut = context + "codec_contract (a positive integer) is required";
                return false;
            }
            d.codecContract = e.at("codec_contract").get<int>();
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
            std::string err;
            const ICodec* codec = active.find(d.codecContract);
            if (!codec) {
                // Fail closed for this design only: an older app keeps decoding
                // the designs it can, and names the one it cannot.
                UnsupportedDesign u{d.id, d.name, d.codecContract, p.seed};
                if (!reg.addUnsupported(std::move(u), &err)) {
                    if (errorOut) *errorOut = err;
                    return false;
                }
                continue;
            }
            d.codebook = std::make_shared<const Codebook>(codec->encode(p, std::move(chips), d.id));
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

bool Registry::loadFile(const std::string& path, Registry& out, std::string* errorOut,
                        const CodecSet* codecs) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (errorOut) *errorOut = "cannot open " + path;
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return parse(ss.str(), out, errorOut, codecs);
}

} // namespace backend::dotgrid
