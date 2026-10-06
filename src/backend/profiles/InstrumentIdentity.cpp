#include "backend/profiles/InstrumentIdentity.h"

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
#include <system_error>

namespace backend::profiles {
namespace {
using Json = nlohmann::json;
constexpr const char* kFile = "instrument_identity.json";

std::string newUuidV4Impl() {
    // IDs become server primary keys: draw every byte from the OS entropy
    // source, mixed with a clock-seeded generator in case random_device is
    // deterministic on some platform.
    std::random_device rd;
    static thread_local std::mt19937_64 mix(
        static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
        (static_cast<uint64_t>(rd()) << 32));
    std::array<unsigned char, 16> b{};
    for (auto& byte : b) byte = static_cast<unsigned char>((rd() ^ mix()) & 0xffu);
    b[6] = static_cast<unsigned char>((b[6] & 0x0fu) | 0x40u); // version 4
    b[8] = static_cast<unsigned char>((b[8] & 0x3fu) | 0x80u); // RFC 4122 variant
    char out[37];
    std::snprintf(out, sizeof(out),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0],
                  b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13],
                  b[14], b[15]);
    return out;
}

bool writeAtomically(const std::filesystem::path& path, const std::string& text) {
    std::error_code ec;
    const auto tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << text;
        if (!out.good()) return false;
    }
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}
} // namespace

std::string generateUuidV4() { return newUuidV4Impl(); }

bool isInstrumentUuid(const std::string& id) {
    if (id.size() != 36) return false;
    for (std::size_t i = 0; i < id.size(); ++i) {
        const char c = id[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return id[14] == '4' && (id[19] == '8' || id[19] == '9' || id[19] == 'a' || id[19] == 'b');
}

InstrumentIdentity loadOrCreateInstrumentIdentity(const std::filesystem::path& dataDir,
                                                  const std::string& name, std::string* warning) {
    InstrumentIdentity identity;
    identity.name = name;
    std::error_code ec;
    std::filesystem::create_directories(dataDir, ec);
    const auto path = dataDir / kFile;

    if (std::filesystem::exists(path, ec)) {
        // Read in its own scope: Windows cannot rename a file that is still open,
        // and the corrupt file is moved aside below.
        std::stringstream text;
        {
            std::ifstream in(path, std::ios::binary);
            text << in.rdbuf();
        }
        try {
            const auto j = Json::parse(text.str());
            const auto id = j.at("instrument_id").get<std::string>();
            if (isInstrumentUuid(id)) {
                identity.id = id;
                return identity;
            }
        } catch (const Json::exception&) {
        }
        // Move the bad file aside rather than deleting evidence.
        for (int n = 1; n < 1000; ++n) {
            const auto aside = dataDir / (std::string(kFile) + ".corrupt-" + std::to_string(n));
            if (!std::filesystem::exists(aside, ec)) {
                std::filesystem::rename(path, aside, ec);
                break;
            }
        }
        if (warning)
            *warning = "instrument identity file was unreadable; moved aside and replaced "
                       "(earlier local method validations no longer match)";
    }

    const auto id = generateUuidV4();
    const Json doc = {{"schema", 1}, {"instrument_id", id}};
    if (!writeAtomically(path, doc.dump(2) + "\n")) {
        if (warning) *warning = "instrument identity could not be written to " + path.string();
        return identity; // empty id: callers report "unknown instrument"
    }
    identity.id = id;
    return identity;
}

} // namespace backend::profiles
