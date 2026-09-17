// Cross-implementation golden test for the dot-grid codebook: the C++
// generator must produce exactly the sequences the Python mask generator
// (scripts/dot_grid) produces from the same seed, otherwise a mask fabricated
// from Python would not decode in the app. Values pinned in
// scripts/dot_grid/test_dotgrid.py as well.
#include "backend/processing/DotGridCodebook.h"
#include "support/assert.h"
#include "support/tempdir.h"

#include <fstream>
#include <string>

#if __has_include(<nlohmann/json.hpp>)
#include <nlohmann/json.hpp>
#define TEST_HAVE_JSON 1
#else
#define TEST_HAVE_JSON 0
#endif

using backend::dotgrid::Codebook;
using backend::dotgrid::CodebookParams;

int main() {
    CodebookParams p;
    p.seed = 7;
    p.columns = 3700;
    p.rows = 3700;
    p.pitchUm = 30.0;
    p.dotDiameterUm = 12.0;
    p.displacementUm = 5.0;
    const Codebook cb = Codebook::generate(p);
    MIB_REQUIRE(cb.valid(), "generated codebook is valid");

    std::string mns;
    for (int b : cb.mns())
        mns += b ? '1' : '0';
    MIB_EXPECT(mns == "000001000011000101001111010001110010010110111011001101010111111",
               "m-sequence golden");

    const int phiGolden[12] = {0, 51, 23, 58, 18, 37, 5, 7, 34, 51, 13, 9};
    const int psiGolden[12] = {0, 61, 35, 11, 25, 47, 24, 21, 19, 42, 57, 53};
    for (int i = 0; i < 12; ++i) {
        MIB_EXPECT(cb.phi()[i] == phiGolden[i], "phi golden at " + std::to_string(i));
        MIB_EXPECT(cb.psi()[i] == psiGolden[i], "psi golden at " + std::to_string(i));
    }
    MIB_EXPECT(cb.phi()[3699] == 42, "phi last golden");
    MIB_EXPECT(cb.psi()[3699] == 1, "psi last golden");
    MIB_EXPECT(cb.direction(100, 200) == std::make_pair(1, 0), "direction golden");
    const auto d = cb.dotUm(100, 200);
    MIB_EXPECT(d.first == 3005.0 && d.second == 6000.0, "dot position golden");

    // Lookup tables invert the phase-difference windows.
    for (int i = 0; i + 2 < p.columns; i += 97) {
        const int d0 = (cb.phi()[i + 1] - cb.phi()[i] + 63) % 63;
        const int d1 = (cb.phi()[i + 2] - cb.phi()[i + 1] + 63) % 63;
        MIB_EXPECT(cb.lookupColumn(d0, d1) == i, "column lookup at " + std::to_string(i));
    }
    for (int j = 0; j + 2 < p.rows; j += 89) {
        const int d0 = (cb.psi()[j + 1] - cb.psi()[j] + 63) % 63;
        const int d1 = (cb.psi()[j + 2] - cb.psi()[j + 1] + 63) % 63;
        MIB_EXPECT(cb.lookupRow(d0, d1) == j, "row lookup at " + std::to_string(j));
    }

    // Direction <-> bits is a bijection over the four axis directions.
    for (int xb = 0; xb < 2; ++xb)
        for (int yb = 0; yb < 2; ++yb) {
            const auto dir = Codebook::directionForBits(xb, yb);
            int x2 = -1, y2 = -1;
            MIB_EXPECT(Codebook::bitsForDirection(dir.first, dir.second, x2, y2),
                       "bits for direction");
            MIB_EXPECT(x2 == xb && y2 == yb, "direction bits round trip");
        }
    int dummyX, dummyY;
    MIB_EXPECT(!Codebook::bitsForDirection(1, 1, dummyX, dummyY),
               "diagonal is not a code direction");

    // Invalid parameters are rejected, never silently accepted.
    bool threw = false;
    try {
        CodebookParams bad = p;
        bad.dotDiameterUm = 25.0; // 2*5 + 25 >= 30
        (void)Codebook::generate(bad);
    } catch (const std::exception&) {
        threw = true;
    }
    MIB_EXPECT(threw, "touching dots are rejected");

    // Chip lookup.
    Codebook withChips = Codebook::generate(p, {{"R0C1", 100.0, 200.0, 300.0, 400.0}});
    MIB_EXPECT(withChips.chipAt(150.0, 250.0) != nullptr &&
                   withChips.chipAt(150.0, 250.0)->name == "R0C1",
               "chip hit");
    MIB_EXPECT(withChips.chipAt(50.0, 250.0) == nullptr, "chip miss");

#if TEST_HAVE_JSON
    // JSON round trip in the scripts/dot_grid codebook.json layout.
    mib::test::TempDir td("dotgrid");
    const auto path = (td / "codebook.json").string();
    {
        nlohmann::json j;
        j["version"] = 1;
        j["seed"] = p.seed;
        j["pitch_um"] = p.pitchUm;
        j["dot_diameter_um"] = p.dotDiameterUm;
        j["displacement_um"] = p.displacementUm;
        j["columns"] = p.columns;
        j["rows"] = p.rows;
        j["origin_um"] = {0.0, 0.0};
        j["mns"] = cb.mns();
        j["phi"] = cb.phi();
        j["psi"] = cb.psi();
        j["design_name"] = "test";
        j["chips"] = nlohmann::json::array({{{"name", "R2C0"},
                                             {"x_min_um", 1.0},
                                             {"y_min_um", 2.0},
                                             {"x_max_um", 3.0},
                                             {"y_max_um", 4.0}}});
        std::ofstream(path) << j.dump();
    }
    Codebook loaded;
    std::string err;
    MIB_REQUIRE(Codebook::loadJson(path, loaded, &err), "codebook.json loads: " + err);
    MIB_EXPECT(loaded.phi() == cb.phi() && loaded.psi() == cb.psi(),
               "loaded phases equal generated");
    MIB_EXPECT(loaded.chips().size() == 1 && loaded.chips()[0].name == "R2C0", "chip table loaded");
    MIB_EXPECT(loaded.designName() == "test", "design name loaded");
    Codebook missing;
    MIB_EXPECT(!Codebook::loadJson((td / "nope.json").string(), missing, &err),
               "missing file fails cleanly");
#endif

    return mib::test::exitCode();
}
