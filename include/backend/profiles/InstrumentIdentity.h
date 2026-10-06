#pragma once
// Stable identity of this instrument PC (#398 M2): a UUID generated once and
// kept in <dataDir>/instrument_identity.json, plus an optional human name
// (MIB_INSTRUMENT_NAME, e.g. "MIB-01"). Local method validations and run
// provenance are keyed by the UUID, so two PCs sharing a name never collide.
// Qt-free; no network.
#include <filesystem>
#include <string>

namespace backend::profiles {

struct InstrumentIdentity {
    std::string id;   // UUID v4, lowercase, 36 characters
    std::string name; // optional label; may be empty
};

// Loads the identity, creating it on first use. An unreadable or malformed
// file is moved aside (instrument_identity.json.corrupt-<n>) and replaced by a
// new identity; `warning` then says so. That direction is safe: validations
// recorded under the old UUID simply stop matching (the method gate warns).
// Returns an identity with an empty id only if the directory is unwritable;
// `warning` explains.
InstrumentIdentity loadOrCreateInstrumentIdentity(const std::filesystem::path& dataDir,
                                                  const std::string& name,
                                                  std::string* warning = nullptr);

// True for a lowercase RFC 4122 v4 UUID string.
bool isInstrumentUuid(const std::string& id);

} // namespace backend::profiles
