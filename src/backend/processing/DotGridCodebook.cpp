#include "backend/processing/DotGridCodebook.h"

#include <spdlog/spdlog.h>

#include <fstream>
#include <stdexcept>
#include <unordered_map>

#if __has_include(<nlohmann/json.hpp>)
#include <nlohmann/json.hpp>
#define MIB_DOTGRID_HAVE_JSON 1
#else
#define MIB_DOTGRID_HAVE_JSON 0
#endif

namespace backend::dotgrid {

namespace {

std::vector<int> mSequence() {
    // x^6 + x^5 + 1 LFSR; every non-zero 6-bit window appears exactly once per period.
    int state[kMnsOrder] = {1, 0, 0, 0, 0, 0};
    std::vector<int> seq;
    seq.reserve(kMnsPeriod);
    for (int n = 0; n < kMnsPeriod; ++n) {
        seq.push_back(state[kMnsOrder - 1]);
        const int fresh = state[kMnsOrder - 1] ^ state[kMnsOrder - 2];
        for (int k = kMnsOrder - 1; k > 0; --k)
            state[k] = state[k - 1];
        state[0] = fresh;
    }
    return seq;
}

uint64_t splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// Symbol sequence in [0, 62] whose kWindowSymbols-windows are unique. Portable
// with the Python generator: same hash, same rejection order.
std::vector<int> deltaSequence(uint64_t seed, uint64_t axis, int count) {
    std::vector<int> deltas;
    deltas.reserve(static_cast<size_t>(count));
    std::unordered_map<int, int> seen; // window key -> start index
    uint64_t attempt = 0;
    int i = 0;
    while (i < count) {
        const uint64_t h = splitmix64((seed * 0x1000193ULL) ^ (axis << 56) ^
                                      (static_cast<uint64_t>(i) << 20) ^ attempt);
        const int d = static_cast<int>(h % static_cast<uint64_t>(kMnsPeriod));
        deltas.push_back(d);
        if (i + 1 >= kWindowSymbols) {
            int key = 0;
            for (int k = i + 1 - kWindowSymbols; k <= i; ++k)
                key = key * kMnsPeriod + deltas[k];
            if (seen.count(key)) {
                deltas.pop_back();
                ++attempt;
                if (attempt > 1000)
                    throw std::runtime_error("dot-grid delta sequence: no unique window found");
                continue;
            }
            seen[key] = i + 1 - kWindowSymbols;
        }
        attempt = 0;
        ++i;
    }
    return deltas;
}

std::vector<int> phasesFromDeltas(const std::vector<int>& deltas) {
    std::vector<int> phases;
    phases.reserve(deltas.size() + 1);
    phases.push_back(0);
    for (int d : deltas)
        phases.push_back((phases.back() + d) % kMnsPeriod);
    return phases;
}

} // namespace

std::pair<int, int> Codebook::directionForBits(int xBit, int yBit) {
    if (yBit == 0) return xBit == 0 ? std::make_pair(1, 0) : std::make_pair(-1, 0);
    return xBit == 0 ? std::make_pair(0, 1) : std::make_pair(0, -1);
}

bool Codebook::bitsForDirection(int dx, int dy, int& xBit, int& yBit) {
    if (dx == 1 && dy == 0) {
        xBit = 0;
        yBit = 0;
        return true;
    }
    if (dx == -1 && dy == 0) {
        xBit = 1;
        yBit = 0;
        return true;
    }
    if (dx == 0 && dy == 1) {
        xBit = 0;
        yBit = 1;
        return true;
    }
    if (dx == 0 && dy == -1) {
        xBit = 1;
        yBit = 1;
        return true;
    }
    return false;
}

Codebook Codebook::generate(const CodebookParams& params, std::vector<Chip> chips) {
    if (params.columns < kWindowDots || params.rows < kWindowDots)
        throw std::invalid_argument("dot-grid codebook: too few columns/rows");
    if (params.displacementUm * 2.0 + params.dotDiameterUm >= params.pitchUm)
        throw std::invalid_argument(
            "dot-grid codebook: dots would touch (2*shift + diameter >= pitch)");
    Codebook cb;
    cb.params_ = params;
    cb.chips_ = std::move(chips);
    cb.mns_ = mSequence();
    cb.phi_ = phasesFromDeltas(deltaSequence(params.seed, 0, params.columns - 1));
    cb.psi_ = phasesFromDeltas(deltaSequence(params.seed, 1, params.rows - 1));
    cb.buildLookups();
    return cb;
}

void Codebook::buildLookups() {
    auto build = [](const std::vector<int>& phases, std::vector<int>& table) {
        table.assign(static_cast<size_t>(kMnsPeriod * kMnsPeriod), -1);
        const int n = static_cast<int>(phases.size()) - 1; // number of deltas
        for (int i = 0; i + kWindowSymbols <= n; ++i) {
            int key = 0;
            for (int k = 0; k < kWindowSymbols; ++k) {
                const int d = (phases[i + k + 1] - phases[i + k] + kMnsPeriod) % kMnsPeriod;
                key = key * kMnsPeriod + d;
            }
            if (table[static_cast<size_t>(key)] != -1)
                throw std::runtime_error("dot-grid codebook: delta windows are not unique");
            table[static_cast<size_t>(key)] = i;
        }
    };
    build(phi_, columnLookup_);
    build(psi_, rowLookup_);
}

bool Codebook::loadJson(const std::string& path, Codebook& out, std::string* errorOut) {
#if MIB_DOTGRID_HAVE_JSON
    try {
        std::ifstream in(path);
        if (!in) {
            if (errorOut) *errorOut = "cannot open " + path;
            return false;
        }
        nlohmann::json j;
        in >> j;
        if (j.value("version", 0) != kCodebookVersion) {
            if (errorOut) *errorOut = "unsupported codebook version";
            return false;
        }
        Codebook cb;
        cb.params_.seed = j.at("seed").get<uint64_t>();
        cb.params_.columns = j.at("columns").get<int>();
        cb.params_.rows = j.at("rows").get<int>();
        cb.params_.pitchUm = j.at("pitch_um").get<double>();
        cb.params_.dotDiameterUm = j.at("dot_diameter_um").get<double>();
        cb.params_.displacementUm = j.at("displacement_um").get<double>();
        const auto& origin = j.at("origin_um");
        cb.params_.originXUm = origin.at(0).get<double>();
        cb.params_.originYUm = origin.at(1).get<double>();
        cb.designName_ = j.value("design_name", std::string{});
        cb.mns_ = j.at("mns").get<std::vector<int>>();
        cb.phi_ = j.at("phi").get<std::vector<int>>();
        cb.psi_ = j.at("psi").get<std::vector<int>>();
        if (cb.mns_.size() != static_cast<size_t>(kMnsPeriod) ||
            cb.phi_.size() != static_cast<size_t>(cb.params_.columns) ||
            cb.psi_.size() != static_cast<size_t>(cb.params_.rows)) {
            if (errorOut) *errorOut = "codebook arrays have inconsistent sizes";
            return false;
        }
        for (const auto& c : j.value("chips", nlohmann::json::array())) {
            Chip chip;
            chip.name = c.at("name").get<std::string>();
            chip.xMinUm = c.at("x_min_um").get<double>();
            chip.yMinUm = c.at("y_min_um").get<double>();
            chip.xMaxUm = c.at("x_max_um").get<double>();
            chip.yMaxUm = c.at("y_max_um").get<double>();
            cb.chips_.push_back(std::move(chip));
        }
        cb.buildLookups();
        out = std::move(cb);
        return true;
    } catch (const std::exception& e) {
        if (errorOut) *errorOut = e.what();
        return false;
    }
#else
    (void)path;
    (void)out;
    if (errorOut) *errorOut = "JSON support not compiled in";
    return false;
#endif
}

std::pair<int, int> Codebook::bits(int i, int j) const {
    const int xBit = mns_[static_cast<size_t>((j + phi_[static_cast<size_t>(i)]) % kMnsPeriod)];
    const int yBit = mns_[static_cast<size_t>((i + psi_[static_cast<size_t>(j)]) % kMnsPeriod)];
    return {xBit, yBit};
}

std::pair<int, int> Codebook::direction(int i, int j) const {
    const auto b = bits(i, j);
    return directionForBits(b.first, b.second);
}

std::pair<double, double> Codebook::nodeUm(int i, int j) const {
    return {params_.originXUm + i * params_.pitchUm, params_.originYUm + j * params_.pitchUm};
}

std::pair<double, double> Codebook::dotUm(int i, int j) const {
    const auto d = direction(i, j);
    const auto n = nodeUm(i, j);
    return {n.first + d.first * params_.displacementUm,
            n.second + d.second * params_.displacementUm};
}

int Codebook::lookupColumn(int d0, int d1) const {
    return columnLookup_[static_cast<size_t>(d0 * kMnsPeriod + d1)];
}

int Codebook::lookupRow(int d0, int d1) const {
    return rowLookup_[static_cast<size_t>(d0 * kMnsPeriod + d1)];
}

const Chip* Codebook::chipAt(double xUm, double yUm) const {
    for (const auto& c : chips_) {
        if (xUm >= c.xMinUm && xUm <= c.xMaxUm && yUm >= c.yMinUm && yUm <= c.yMaxUm) return &c;
    }
    return nullptr;
}

} // namespace backend::dotgrid
