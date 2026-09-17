#pragma once

// Dot-grid wafer localization codebook (Qt-free, portable).
//
// The mask carries a square lattice of dots; each dot is displaced from its
// lattice node in one of four directions, encoding an x-plane bit and a
// y-plane bit. Column i of the x-plane holds a period-63 m-sequence cyclically
// shifted by phi[i]; row j of the y-plane holds the same sequence shifted by
// psi[j]. Phase differences between neighbouring columns (rows) form a random
// symbol sequence whose 2-symbol windows are unique, so three consecutive
// columns with known phase identify the absolute column, and likewise for
// rows. The Python reference (scripts/dot_grid/dotgrid/codebook.py) generates
// the identical codebook from the same seed; processing.dot_grid_codebook pins
// the cross-implementation golden values.
//
// See docs/architecture/dot-grid-localization.md.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace backend::dotgrid {

constexpr int kMnsOrder = 6;
constexpr int kMnsPeriod = 63;    // (1 << kMnsOrder) - 1
constexpr int kWindowSymbols = 2; // phase differences per lookup
constexpr int kWindowDots = 3;    // consecutive lines needed along the coded axis
constexpr int kCodebookVersion = 1;

struct Chip {
    std::string name;
    double xMinUm{0.0};
    double yMinUm{0.0};
    double xMaxUm{0.0};
    double yMaxUm{0.0};
};

struct CodebookParams {
    uint64_t seed{7};
    int columns{3700};
    int rows{3700};
    double pitchUm{30.0};
    double dotDiameterUm{12.0};
    double displacementUm{5.0};
    double originXUm{0.0};
    double originYUm{0.0};
};

class Codebook {
public:
    Codebook() = default;

    // Deterministic generation from the seed (matches the Python generator).
    static Codebook generate(const CodebookParams& params, std::vector<Chip> chips = {});

    // Load a codebook.json written by scripts/dot_grid (includes chip table).
    // Returns false and fills errorOut when the file is missing or malformed.
    static bool loadJson(const std::string& path, Codebook& out, std::string* errorOut = nullptr);

    bool valid() const { return params_.columns > 0 && params_.rows > 0 && !mns_.empty(); }
    const CodebookParams& params() const { return params_; }
    const std::vector<Chip>& chips() const { return chips_; }
    const std::string& designName() const { return designName_; }

    const std::vector<int>& mns() const { return mns_; }
    const std::vector<int>& phi() const { return phi_; }
    const std::vector<int>& psi() const { return psi_; }

    // (x_bit, y_bit) at lattice node (column i, row j).
    std::pair<int, int> bits(int i, int j) const;
    // Unit displacement direction of the dot at node (i, j).
    std::pair<int, int> direction(int i, int j) const;
    // Nominal node and actual dot centre in wafer micrometres.
    std::pair<double, double> nodeUm(int i, int j) const;
    std::pair<double, double> dotUm(int i, int j) const;

    // Absolute column (row) index whose phase-difference window is (d0, d1); -1 if none.
    int lookupColumn(int d0, int d1) const;
    int lookupRow(int d0, int d1) const;

    // Chip containing the wafer point, or nullptr.
    const Chip* chipAt(double xUm, double yUm) const;

    // Direction encoding shared with the decoder: bits -> unit displacement.
    static std::pair<int, int> directionForBits(int xBit, int yBit);
    // Inverse; returns false when (dx, dy) is not a unit axis vector.
    static bool bitsForDirection(int dx, int dy, int& xBit, int& yBit);

private:
    void buildLookups();

    CodebookParams params_;
    std::vector<Chip> chips_;
    std::string designName_;
    std::vector<int> mns_;
    std::vector<int> phi_;
    std::vector<int> psi_;
    std::vector<int> columnLookup_; // 63*63 entries, -1 = unused
    std::vector<int> rowLookup_;
};

} // namespace backend::dotgrid
