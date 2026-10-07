// update_catalog_test
//
// Pins the per-channel update index parse, newest-first ordering (including
// -beta.N vs its release), bad-entry skipping, and the downgrade check that
// gates rollback in the Software Updates dialog.

#include "frontend/utils/UpdateCatalog.h"

#include "support/assert.h"

#include <QByteArray>
#include <QFile>
#include <QString>

namespace uc = frontend::updatecatalog;

namespace {
uc::ParseResult parse(const char* j) { return uc::parseIndex(QByteArray(j)); }
} // namespace

int main()
{
    // Valid index: two versions, returned newest-first with fields carried.
    {
        const auto r = parse(R"json({"schema_version":1,"channel":"beta","versions":[
            {"version":"1.0.3","installer_url":"https://updates.yofo.bio/beta/a.exe","installer_sha256":"AA","installer_size_bytes":10,"release_notes_url":"https://x/3","published_utc":"2026-06-01T00:00:00Z"},
            {"version":"1.0.4-beta.1","installer_url":"https://updates.yofo.bio/beta/b.exe","installer_sha256":"bb","installer_size_bytes":20,"release_notes":"## Highlights\nOffline Help (#573)","release_notes_url":"https://x/4b1","published_utc":"2026-06-24T00:00:00Z"}
        ]})json");
        MIB_REQUIRE(r.ok, "valid index parses");
        MIB_REQUIRE(r.versions.size() == 2, "two entries");
        MIB_EXPECT(r.versions[0].version == "1.0.4-beta.1", "beta of 1.0.4 sorts above 1.0.3");
        MIB_EXPECT(r.versions[0].installerSizeBytes == 20, "size carried");
        MIB_EXPECT(r.versions[0].installerSha256Hex == "bb", "sha carried");
        MIB_EXPECT(r.versions[0].releaseNotes == "## Highlights\nOffline Help (#573)",
                   "inline notes carried");
        MIB_EXPECT(r.versions[0].releaseNotesUrl == "https://x/4b1", "notes url carried");
        MIB_EXPECT(r.versions[1].version == "1.0.3", "older last");
        MIB_EXPECT(r.versions[1].installerSha256Hex == "aa", "sha lowercased");
    }

    // beta vs its release: 1.0.4 release sorts above 1.0.4-beta.1.
    {
        const auto r = parse(R"({"versions":[
            {"version":"1.0.4-beta.1","installer_url":"u","installer_sha256":"s"},
            {"version":"1.0.4","installer_url":"u","installer_sha256":"s"}
        ]})");
        MIB_REQUIRE(r.ok && r.versions.size() == 2, "parses");
        MIB_EXPECT(r.versions[0].version == "1.0.4", "release above its beta");
    }

    // Bad entries skipped; non-object/array/malformed rejected.
    {
        const auto r = parse(R"({"versions":[{"version":"1.0.0"},{"version":"1.0.1","installer_url":"u","installer_sha256":"s"}]})");
        MIB_EXPECT(r.ok && r.versions.size() == 1 && r.versions[0].version == "1.0.1",
                   "entry missing url/sha is skipped");
        MIB_EXPECT(!parse("[1,2]").ok, "array root rejected");
        MIB_EXPECT(!parse("{ bad json").ok, "malformed rejected");
        const auto empty = parse(R"({"versions":[]})");
        MIB_EXPECT(empty.ok && empty.versions.isEmpty(), "empty versions array is valid+empty");
    }

    // indexOfVersion + isDowngrade.
    {
        const auto r = parse(R"({"versions":[
            {"version":"1.0.4","installer_url":"u","installer_sha256":"s"},
            {"version":"1.0.3","installer_url":"u","installer_sha256":"s"}
        ]})");
        MIB_EXPECT(uc::indexOfVersion(r.versions, "1.0.3") == 1, "finds current");
        MIB_EXPECT(uc::indexOfVersion(r.versions, "9.9.9") == -1, "absent -> -1");
        MIB_EXPECT(uc::isDowngrade("1.0.3", "1.0.4"), "older is downgrade");
        MIB_EXPECT(!uc::isDowngrade("1.0.4", "1.0.3"), "newer is not");
        MIB_EXPECT(!uc::isDowngrade("1.0.4", "1.0.4"), "same version is not a downgrade");
        MIB_EXPECT(uc::isDowngrade("1.0.4-beta.1", "1.0.4"), "beta of installed release is a downgrade");
    }

    // channelForVersion: a build must know its channel from its own version
    // string (the bug: stripped versions made stable and beta indistinguishable).
    {
        MIB_EXPECT(uc::channelForVersion("1.0.4") == "stable", "release -> stable");
        MIB_EXPECT(uc::channelForVersion("1.0.4-beta.1") == "beta", "beta suffix -> beta");
        MIB_EXPECT(uc::channelForVersion("1.0.4-beta.12") == "beta", "multi-digit beta -> beta");
        MIB_EXPECT(uc::channelForVersion("") == "stable", "empty -> stable");
    }

    // Boundary test: parse a fixture produced by publish-update.py's real
    // transform (_index_entry_from_manifest + merge_index). This proves the
    // JSON the publisher emits is exactly what the app reads — guards against
    // field-name drift across the Python/C++ boundary.
    {
        QFile f(QStringLiteral("%1/tests/frontend/fixtures/sample_index.json")
                    .arg(QStringLiteral(MIB_PROJECT_SOURCE_DIR)));
        MIB_REQUIRE(f.open(QIODevice::ReadOnly), "fixture sample_index.json opens");
        const auto r = uc::parseIndex(f.readAll());
        MIB_REQUIRE(r.ok, "publish-produced index parses");
        MIB_REQUIRE(r.versions.size() == 3, "three versions in fixture");
        MIB_EXPECT(r.versions[0].version == "1.0.4", "release first");
        MIB_EXPECT(r.versions[1].version == "1.0.4-beta.1", "beta second");
        MIB_EXPECT(r.versions[2].version == "1.0.3", "older last");
        MIB_EXPECT(r.versions[0].installerSizeBytes == 43600452, "size field read");
        MIB_EXPECT(!r.versions[0].installerSha256Hex.isEmpty(), "sha field read");
        MIB_EXPECT(!r.versions[0].publishedUtc.isEmpty(), "published_utc field read");
        MIB_EXPECT(r.versions[0].installerUrl.startsWith("https://updates.yofo.bio/"),
                   "installer_url field read");
    }

    if (mib::test::exitCode() == 0) {
        std::printf("UpdateCatalog parse/sort/downgrade + publish-fixture round-trip verified\n");
    }
    return mib::test::exitCode();
}
