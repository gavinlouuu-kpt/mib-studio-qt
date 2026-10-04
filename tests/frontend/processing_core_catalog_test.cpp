#include "frontend/utils/ProcessingCoreCatalog.h"
#include "support/assert.h"

int main() {
    const QByteArray fixture = R"({
      "processing_core_index_schema_version": 1,
      "channel": "stable",
      "active_version": "2.0.0",
      "versions": [
        {"version":"2.0.0","contract_version":1,"published_at":"2026-07-13T00:00:00Z",
         "release_tag":"mib-processing-v2.0.0","release_url":"https://example/release",
         "manifest_url":"https://updates.example/stable/processing-core/versions/2.0.0.json",
         "native_plugins":[{"filename":"core.dll","os":"windows","arch":"x86_64",
          "url":"https://example/core.dll","sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
          "size_bytes":42,"engine_abi_version":1,"contract_version":1,
          "runtime_fingerprint":"windows-x86_64-msvc1942-md-cxx17","entrypoint":"mib_processing_get_api",
          "app_min_version":"1.0.0","app_max_version":"1.9.9",
          "signing":{"scheme":"authenticode","required":true}}]},
        {"version":"1.0.0","contract_version":1,
         "manifest_url":"https://updates.example/stable/processing-core/versions/1.0.0.json",
         "native_plugins":[]}
      ]})";
    const auto parsed = frontend::processingcorecatalog::parseIndex(fixture);
    MIB_REQUIRE(parsed.ok, parsed.error.toStdString());
    MIB_EXPECT(parsed.channel == "stable" && parsed.indexActiveVersion == "2.0.0" &&
                   parsed.activeVersion.isEmpty(),
               "index active field is advisory until latest.json is validated");
    MIB_EXPECT(parsed.versions.size() == 2, "history entries retained");
    const auto* plugin = frontend::processingcorecatalog::findNativePlugin(
        parsed.versions.front(), "windows", "x86_64");
    MIB_REQUIRE(plugin != nullptr, "compatible Windows artifact selected");
    MIB_EXPECT(plugin->engineAbiVersion == 1 && plugin->contractVersion == 1,
               "native compatibility metadata parsed");
    MIB_EXPECT(plugin->signingScheme == "authenticode" && plugin->signingRequired,
               "platform trust policy is part of the selected artifact identity");
    MIB_EXPECT(frontend::processingcorecatalog::isAppCompatible(*plugin, "1.2.3"),
               "app compatibility range accepts supported version");
    MIB_EXPECT(!frontend::processingcorecatalog::isAppCompatible(*plugin, "2.0.0"),
               "app compatibility range rejects unsupported version");
    MIB_EXPECT(frontend::processingcorecatalog::isProcessingContractCompatible(0, 1) &&
                   frontend::processingcorecatalog::isProcessingContractCompatible(1, 1) &&
                   !frontend::processingcorecatalog::isProcessingContractCompatible(2, 1),
               "optional profile processing contract is advisory unless present and different");
    MIB_EXPECT(frontend::processingcorecatalog::isVersionDowngrade("1.9.9", "2.0.0") &&
                   frontend::processingcorecatalog::isVersionDowngrade("2.0.0rc1", "2.0.0") &&
                   !frontend::processingcorecatalog::isVersionDowngrade("2.1.0", "2.0.0"),
               "core downgrade detection handles older releases and prereleases");
    auto unboundedPlugin = *plugin;
    unboundedPlugin.appMaxVersion.clear();
    MIB_EXPECT(frontend::processingcorecatalog::isAppCompatible(unboundedPlugin, "99.0.0"),
               "null app maximum is treated as an unbounded range");
    MIB_EXPECT(frontend::processingcorecatalog::findNativePlugin(
                   parsed.versions.front(), "linux", "x86_64") == nullptr,
               "missing platform does not silently fall back");
    MIB_EXPECT(!frontend::processingcorecatalog::parseIndex("not-json").ok,
               "malformed catalog rejected");
    const QByteArray manifest = R"({
      "processing_core_manifest_schema_version":2,"channel":"stable",
      "version":"2.0.0","contract_version":1,
      "published_at":"2026-07-13T00:00:00Z",
      "wheel":{"version":"2.0.0","release_tag":"mib-processing-v2.0.0","release_url":"https://example/release"},
      "native_plugins":[{"filename":"core.dll","os":"windows","arch":"x86_64",
       "url":"https://example/core.dll","sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
       "size_bytes":42,"engine_abi_version":1,"contract_version":1,
       "runtime_fingerprint":"windows-x86_64-msvc1942-md-cxx17","entrypoint":"mib_processing_get_api",
       "app_min_version":"1.0.0","app_max_version":"1.9.9",
       "signing":{"scheme":"authenticode","required":true}}]})";
    const auto parsedManifest = frontend::processingcorecatalog::parseVersionManifest(manifest);
    MIB_REQUIRE(parsedManifest.ok, parsedManifest.error.toStdString());
    MIB_EXPECT(parsedManifest.version.version == "2.0.0" &&
                   parsedManifest.rawSha256Hex.size() == 64,
               "immutable manifest identity and raw digest parsed");
    const auto canonical = frontend::processingcorecatalog::validateCanonicalActive(
        parsed, parsedManifest);
    MIB_REQUIRE(canonical.ok, canonical.error.toStdString());
    MIB_EXPECT(canonical.version == "2.0.0" && canonical.warning.isEmpty(),
               "latest.json supplies the canonical channel-active version");

    QByteArray partiallyPublished = fixture;
    partiallyPublished.replace("\"active_version\": \"2.0.0\"",
                               "\"active_version\": \"1.0.0\"");
    const auto partialIndex = frontend::processingcorecatalog::parseIndex(partiallyPublished);
    MIB_REQUIRE(partialIndex.ok, partialIndex.error.toStdString());
    const auto partialCanonical = frontend::processingcorecatalog::validateCanonicalActive(
        partialIndex, parsedManifest);
    MIB_EXPECT(partialCanonical.ok && partialCanonical.version == "2.0.0" &&
                   !partialCanonical.warning.isEmpty(),
               "index active disagreement cannot lead the canonical latest pointer");

    QByteArray alteredLatest = manifest;
    alteredLatest.replace("\"size_bytes\":42", "\"size_bytes\":43");
    const auto inconsistentLatest = frontend::processingcorecatalog::parseVersionManifest(
        alteredLatest);
    MIB_REQUIRE(inconsistentLatest.ok, inconsistentLatest.error.toStdString());
    MIB_EXPECT(!frontend::processingcorecatalog::validateCanonicalActive(
                    parsed, inconsistentLatest).ok,
               "latest pointer metadata must agree with immutable history");

    QByteArray duplicate = fixture;
    duplicate.replace("\"version\":\"1.0.0\"", "\"version\":\"2.0.0\"");
    MIB_EXPECT(!frontend::processingcorecatalog::parseIndex(duplicate).ok,
               "duplicate versions rejected");
    QByteArray unsafe = fixture;
    unsafe.replace("\"active_version\": \"2.0.0\"", "\"active_version\": \"../2\"");
    MIB_EXPECT(!frontend::processingcorecatalog::parseIndex(unsafe).ok,
               "unsafe active version rejected");
    QByteArray nonHex = fixture;
    nonHex.replace(QByteArray(64, 'a'), QByteArray(64, 'g'));
    MIB_EXPECT(!frontend::processingcorecatalog::parseIndex(nonHex).ok,
               "non-hex native digest rejected");
    QByteArray optionalSignature = fixture;
    optionalSignature.replace("\"required\":true", "\"required\":false");
    MIB_EXPECT(!frontend::processingcorecatalog::parseIndex(optionalSignature).ok,
               "optional native signature policy is rejected");
    QByteArray missingManifestUrl = fixture;
    missingManifestUrl.replace(
        "\"manifest_url\":\"https://updates.example/stable/processing-core/versions/2.0.0.json\",",
        "");
    MIB_EXPECT(!frontend::processingcorecatalog::parseIndex(missingManifestUrl).ok,
               "history entry without immutable manifest URL rejected");

    const QByteArray duplicatePlatform = R"({
      "processing_core_index_schema_version":1,"channel":"stable","active_version":"2.0.0",
      "versions":[{"version":"2.0.0","contract_version":1,
       "manifest_url":"https://updates.example/stable/processing-core/versions/2.0.0.json",
       "native_plugins":[
        {"filename":"a.dll","os":"windows","arch":"x86_64","url":"https://example/a.dll",
         "sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
         "size_bytes":1,"engine_abi_version":1,"contract_version":1,
         "runtime_fingerprint":"runtime","entrypoint":"mib_processing_get_api",
         "app_min_version":"1.0.0","app_max_version":null,
         "signing":{"scheme":"authenticode","required":true}},
        {"filename":"b.dll","os":"windows","arch":"x86_64","url":"https://example/b.dll",
         "sha256":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
         "size_bytes":1,"engine_abi_version":1,"contract_version":1,
         "runtime_fingerprint":"runtime","entrypoint":"mib_processing_get_api",
         "app_min_version":"1.0.0","app_max_version":null,
         "signing":{"scheme":"authenticode","required":true}}]}]})";
    MIB_EXPECT(!frontend::processingcorecatalog::parseIndex(duplicatePlatform).ok,
               "duplicate native platform entries rejected");

    const QByteArray linuxCatalog = R"({
      "processing_core_index_schema_version":1,"channel":"stable","active_version":"2.0.0",
      "versions":[{"version":"2.0.0","contract_version":1,
       "manifest_url":"https://updates.example/stable/processing-core/versions/2.0.0.json",
       "native_plugins":[{"filename":"core.so","os":"linux","arch":"amd64",
        "url":"https://example/core.so",
        "sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "size_bytes":1,"engine_abi_version":1,"contract_version":1,
        "runtime_fingerprint":"linux-x86_64-gcc13-cxx17",
        "entrypoint":"mib_processing_get_api","app_min_version":"1.0.0",
        "app_max_version":null,
        "signing":{"scheme":"detached-ed25519","required":true}}]}]})";
    const auto parsedLinux = frontend::processingcorecatalog::parseIndex(linuxCatalog);
    MIB_REQUIRE(parsedLinux.ok, parsedLinux.error.toStdString());
    const auto* linuxPlugin = frontend::processingcorecatalog::findNativePlugin(
        parsedLinux.versions.front(), "linux", "x86_64");
    MIB_REQUIRE(linuxPlugin != nullptr, "Linux shared-library artifact selected exactly");
    MIB_EXPECT(linuxPlugin->signingScheme == "detached-ed25519" &&
                   linuxPlugin->signingRequired,
               "Linux trust scheme is preserved without assuming Authenticode");

    // The production "ed25519" scheme must carry canonical detached-signature
    // transport: 44-byte DER SPKI, its 64-hex SHA-256, and a 64-byte signature.
    const QString spkiBase64 = QString::fromLatin1(QByteArray(44, '\x01').toBase64());
    const QString signatureBase64 = QString::fromLatin1(QByteArray(64, '\x02').toBase64());
    const QString ed25519Template = QStringLiteral(R"({
      "processing_core_index_schema_version":1,"channel":"stable","active_version":"2.0.0",
      "versions":[{"version":"2.0.0","contract_version":1,
       "manifest_url":"https://updates.example/stable/processing-core/versions/2.0.0.json",
       "native_plugins":[{"filename":"core.so","os":"linux","arch":"x86_64",
        "url":"https://example/core.so",
        "sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "size_bytes":1,"engine_abi_version":1,"contract_version":1,
        "runtime_fingerprint":"linux-x86_64-gcc13-cxx17",
        "entrypoint":"mib_processing_get_api","app_min_version":"1.0.0",
        "app_max_version":null,
        "signing":{"scheme":"ed25519","required":true%1}}]}]})");
    const QString ed25519Fields = QStringLiteral(
        R"(,"public_key_spki_base64":"%1",)"
        R"("public_key_spki_sha256":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",)"
        R"("signature_base64":"%2")")
                                        .arg(spkiBase64, signatureBase64);
    const auto parsedEd25519 = frontend::processingcorecatalog::parseIndex(
        ed25519Template.arg(ed25519Fields).toUtf8());
    MIB_REQUIRE(parsedEd25519.ok, parsedEd25519.error.toStdString());
    const auto* ed25519Plugin = frontend::processingcorecatalog::findNativePlugin(
        parsedEd25519.versions.front(), "linux", "x86_64");
    MIB_REQUIRE(ed25519Plugin != nullptr, "ed25519 shared-library artifact selected");
    MIB_EXPECT(ed25519Plugin->signingPublicKeySpkiBase64 == spkiBase64 &&
                   ed25519Plugin->signingSignatureBase64 == signatureBase64 &&
                   ed25519Plugin->signingPublicKeySpkiSha256 ==
                       QString(64, QLatin1Char('b')),
               "ed25519 detached-signature transport fields are preserved");

    const auto missingSignature =
        frontend::processingcorecatalog::parseIndex(ed25519Template.arg(QString()).toUtf8());
    MIB_EXPECT(!missingSignature.ok,
               "an ed25519 entry without detached-signature material is rejected");

    const QString truncatedFields = QStringLiteral(
        R"(,"public_key_spki_base64":"%1",)"
        R"("public_key_spki_sha256":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",)"
        R"("signature_base64":"%2")")
                                         .arg(spkiBase64,
                                              QString::fromLatin1(QByteArray(63, '\x02').toBase64()));
    const auto truncatedSignature = frontend::processingcorecatalog::parseIndex(
        ed25519Template.arg(truncatedFields).toUtf8());
    MIB_EXPECT(!truncatedSignature.ok,
               "an ed25519 entry with a non-canonical signature length is rejected");
    // ---- absdiff-laplacian line (native-only, ABI 2, Contract 2) ----------------
    const QByteArray linePlugin = R"({"filename":"mib_processing_core-absdiff-laplacian-0.1.0-windows_x86_64.dll",
       "os":"windows","arch":"x86_64","url":"https://example/c2.dll",
       "sha256":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
       "size_bytes":7,"engine_abi_version":2,"contract_version":2,
       "runtime_fingerprint":"windows-x86_64-msvc1942-md-cxx17","entrypoint":"mib_processing_get_api_v2",
       "app_min_version":"1.0.0","app_max_version":null,"signing":{"scheme":"authenticode","required":true}})";
    const QByteArray lineIndex = R"({"processing_core_line_index_schema_version":1,"line":"absdiff-laplacian",
       "channel":"beta","active_version":"0.1.0","versions":[{"version":"0.1.0","contract_version":2,
       "engine_abi_version":2,"published_at":"2026-10-04T00:00:00Z",
       "release_tag":"mib-processing-absdiff-laplacian-v0.1.0","release_url":"https://example/r",
       "manifest_url":"https://updates.example/beta/processing-core/absdiff-laplacian/versions/0.1.0.json",
       "native_plugins":[)" + linePlugin + R"(]}]})";
    const auto lineParsed = frontend::processingcorecatalog::parseIndex(lineIndex);
    MIB_REQUIRE(lineParsed.ok, lineParsed.error.toStdString());
    MIB_EXPECT(lineParsed.line == "absdiff-laplacian" && lineParsed.versions.front().line == "absdiff-laplacian" &&
                   lineParsed.versions.front().engineAbiVersion == 2,
               "line index carries its line and ABI");
    MIB_EXPECT(parsed.line == "subtract-ring" && parsed.versions.front().line == "subtract-ring" &&
                   parsed.versions.front().engineAbiVersion == 1,
               "legacy index is the subtract-ring line at ABI 1");
    const QByteArray lineManifest = R"({"processing_core_line_manifest_schema_version":1,"line":"absdiff-laplacian",
       "channel":"beta","version":"0.1.0","published_at":"2026-10-04T00:00:00Z","contract_version":2,
       "engine_abi_version":2,"release_tag":"mib-processing-absdiff-laplacian-v0.1.0","release_url":"https://example/r",
       "manifest_url":"https://updates.example/beta/processing-core/absdiff-laplacian/versions/0.1.0.json",
       "native_plugins":[)" + linePlugin + R"(]})";
    const auto lineLatest = frontend::processingcorecatalog::parseVersionManifest(lineManifest);
    MIB_REQUIRE(lineLatest.ok, lineLatest.error.toStdString());
    MIB_EXPECT(lineLatest.version.releaseTag == "mib-processing-absdiff-laplacian-v0.1.0" &&
                   lineLatest.version.line == "absdiff-laplacian",
               "line manifest identity parsed from top-level fields");
    const auto lineActive = frontend::processingcorecatalog::validateCanonicalActive(lineParsed, lineLatest);
    MIB_EXPECT(lineActive.ok && lineActive.version == "0.1.0", "line latest.json validates against its index");
    QByteArray wrongEntry = lineIndex;
    wrongEntry.replace("\"entrypoint\":\"mib_processing_get_api_v2\"", "\"entrypoint\":\"mib_processing_get_api\"");
    MIB_EXPECT(!frontend::processingcorecatalog::parseIndex(wrongEntry).ok,
               "an ABI-2 plugin with the v1 entrypoint is refused");
    QByteArray noLine = lineIndex;
    noLine.replace("\"line\":\"absdiff-laplacian\",", "");
    MIB_EXPECT(!frontend::processingcorecatalog::parseIndex(noLine).ok, "a line index without its line is refused");
    return mib::test::exitCode();
}
