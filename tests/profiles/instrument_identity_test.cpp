// instrument_identity_test (issue #398 M2)
//
// The instrument PC identity that local method validations and run
// provenance are keyed by:
//  - generated once as a lowercase v4 UUID and stable across restarts;
//  - the name comes from the caller each time (not persisted);
//  - a malformed or non-UUID file is moved aside (evidence kept) and replaced,
//    with a warning;
//  - an unwritable data dir yields an empty id and a warning, never a throw.
// (AppBackend wiring is covered by registry_facade_test.)
#include "backend/profiles/InstrumentIdentity.h"

#include "support/assert.h"
#include "support/tempdir.h"

#include <fstream>
#include <set>
#include <sstream>

using backend::profiles::isInstrumentUuid;
using backend::profiles::loadOrCreateInstrumentIdentity;

namespace {

void writeText(const std::filesystem::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string readText(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

} // namespace

int main() {
    mib::test::TempDir scratch("mib_instrument_identity");

    // UUID shape check.
    MIB_EXPECT(isInstrumentUuid("123e4567-e89b-42d3-a456-426614174000"), "valid v4 accepted");
    MIB_EXPECT(!isInstrumentUuid("123e4567-e89b-12d3-a456-426614174000"), "v1 rejected");
    MIB_EXPECT(!isInstrumentUuid("123E4567-E89B-42D3-A456-426614174000"), "uppercase rejected");
    MIB_EXPECT(!isInstrumentUuid("123e4567-e89b-42d3-c456-426614174000"), "bad variant rejected");
    MIB_EXPECT(!isInstrumentUuid("MIB-01"), "a name is not an id");
    MIB_EXPECT(!isInstrumentUuid(""), "empty rejected");

    // First use creates; later loads return the same id.
    const auto dataDir = scratch / "data";
    std::string warning;
    const auto first = loadOrCreateInstrumentIdentity(dataDir, "MIB-01", &warning);
    MIB_EXPECT(isInstrumentUuid(first.id), "fresh id is a v4 UUID");
    MIB_EXPECT(first.name == "MIB-01", "name carried");
    MIB_EXPECT(warning.empty(), "no warning on first creation");
    MIB_EXPECT(std::filesystem::exists(dataDir / "instrument_identity.json"), "identity file written");
    MIB_EXPECT(!std::filesystem::exists(dataDir / "instrument_identity.json.tmp"), "no temp file left");

    const auto again = loadOrCreateInstrumentIdentity(dataDir, "", &warning);
    MIB_EXPECT(again.id == first.id, "id stable across restarts");
    MIB_EXPECT(again.name.empty(), "name is not persisted");
    MIB_EXPECT(warning.empty(), "no warning on a clean reload");
    MIB_EXPECT(readText(dataDir / "instrument_identity.json").find("MIB-01") == std::string::npos,
               "name not written to the identity file");

    // Two data dirs never share an id.
    std::set<std::string> ids;
    for (int i = 0; i < 16; ++i)
        ids.insert(loadOrCreateInstrumentIdentity(scratch / ("pc" + std::to_string(i)), "MIB-01").id);
    MIB_EXPECT(ids.size() == 16, "distinct instruments get distinct ids even with the same name");

    // Malformed JSON is moved aside and replaced.
    writeText(dataDir / "instrument_identity.json", "{not json");
    warning.clear();
    const auto replaced = loadOrCreateInstrumentIdentity(dataDir, "MIB-01", &warning);
    MIB_EXPECT(isInstrumentUuid(replaced.id) && replaced.id != first.id, "corrupt file replaced by a new id");
    MIB_EXPECT(warning.find("moved aside") != std::string::npos, "warning explains the replacement");
    MIB_EXPECT(readText(dataDir / "instrument_identity.json.corrupt-1") == "{not json",
               "corrupt file kept as evidence");

    // A well-formed file whose id is not a UUID is treated the same way.
    writeText(dataDir / "instrument_identity.json", R"({"schema":1,"instrument_id":"MIB-01"})");
    warning.clear();
    const auto renamed = loadOrCreateInstrumentIdentity(dataDir, "", &warning);
    MIB_EXPECT(isInstrumentUuid(renamed.id), "non-UUID id replaced");
    MIB_EXPECT(!warning.empty(), "warned");
    MIB_EXPECT(std::filesystem::exists(dataDir / "instrument_identity.json.corrupt-2"),
               "second corrupt file gets the next suffix");

    // Unwritable data dir: a regular file stands where the directory should be.
    writeText(scratch / "blocker", "x");
    warning.clear();
    const auto none = loadOrCreateInstrumentIdentity(scratch / "blocker" / "data", "MIB-01", &warning);
    MIB_EXPECT(none.id.empty(), "no id when the data dir cannot be written");
    MIB_EXPECT(none.name == "MIB-01", "name still reported");
    MIB_EXPECT(warning.find("could not be written") != std::string::npos, "warning explains");

    return mib::test::exitCode();
}
