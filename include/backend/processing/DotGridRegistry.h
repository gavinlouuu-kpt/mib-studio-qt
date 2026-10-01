#pragma once

// Dot-grid design registry (Qt-free, portable).
//
// Every chip design that carries a dot grid is registered once with a unique
// seed. The codebook is a pure function of (seed, lattice, geometry, origin),
// so an entry stores only those parameters plus the chip (die) table and the
// codebook is regenerated at load time. A frame decodes under the codebook of
// the design it shows and under no other, so the decoder tries every
// registered design and the winning one identifies the design as well as the
// position. The Python tools (scripts/dot_grid/dotgrid/registry.py) write the
// same file; the bundled copy is resources/defaults/dot_grid/registry.json.
//
// See docs/architecture/dot-grid-localization.md (Design registry).

#include "backend/processing/DotGridCodebook.h"

#include <memory>
#include <string>
#include <vector>

namespace backend::dotgrid {

constexpr int kRegistryVersion = 1;

struct Design {
    std::string id;       // lowercase slug, unique
    std::string name;     // human-readable
    std::string revision; // free text, usually a date
    std::string status;   // "active" | "retired" (both decode: retired wafers still exist)
    std::shared_ptr<const Codebook> codebook;
};

class Registry {
public:
    Registry() = default;

    // Parse a registry.json document. The whole document is rejected (false +
    // errorOut) on a version mismatch, a malformed entry, a duplicate id or
    // seed, or geometry that cannot be fabricated.
    static bool parse(const std::string& jsonText, Registry& out, std::string* errorOut = nullptr);
    static bool loadFile(const std::string& path, Registry& out, std::string* errorOut = nullptr);

    // A one-design registry around an existing codebook (legacy single-codebook mode).
    static Registry single(std::shared_ptr<const Codebook> codebook, std::string id = {},
                           std::string name = {});

    // Adds a design; refuses a duplicate id or seed.
    bool add(Design design, std::string* errorOut = nullptr);

    // Adds every design of `other`. An entry identical to one already present
    // (same id, seed and geometry) is dropped silently; one whose id or seed
    // clashes with a different design is skipped with a warning.
    void merge(const Registry& other, std::vector<std::string>* warnings = nullptr);

    const std::vector<Design>& designs() const { return designs_; }
    bool empty() const { return designs_.empty(); }
    size_t size() const { return designs_.size(); }
    const Design* find(const std::string& id) const;

    // Stable text identity of the contents (ids, seeds, geometry, chip count),
    // used to skip rebuilding decoders when a reloaded config did not change.
    std::string fingerprint() const;

    static bool validId(const std::string& id);

private:
    std::vector<Design> designs_;
};

} // namespace backend::dotgrid
