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
    // ADR 0007: the absdiff-laplacian (Contract 2) line has its own registry
    // directory, a wheel-less manifest with a top-level release identity, and
    // engine-ABI-v2 cores with the v2 entry point.
    namespace cat = frontend::processingcorecatalog;
    MIB_EXPECT(cat::findCoreLine("subtract-ring") &&
                   cat::findCoreLine("subtract-ring")->registryDir == "processing-core" &&
                   cat::findCoreLine("absdiff-laplacian") &&
                   cat::findCoreLine("absdiff-laplacian")->registryDir ==
                       "processing-core-absdiff-laplacian" &&
                   !cat::findCoreLine("unet-cells"),
               "core lines map to their registry directories");
    const QByteArray c2Plugin = R"({"filename":"mib_processing_core-absdiff-laplacian-0.1.0-linux_x86_64.so",
       "os":"linux","arch":"x86_64","algorithm":"absdiff-laplacian",
       "url":"https://example/c2.so","sha256":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
       "size_bytes":64,"engine_abi_version":2,"contract_version":2,
       "runtime_fingerprint":"linux-x86_64-gcc13-cxx17","entrypoint":"mib_processing_get_api_v2",
       "app_min_version":"1.0.0","app_max_version":null,
       "signing":{"scheme":"authenticode","required":true}})";
    const QByteArray c2Manifest = QByteArray(R"({
      "processing_core_manifest_schema_version":2,"channel":"stable","line":"absdiff-laplacian",
      "version":"0.1.0","contract_version":2,"published_at":"2026-10-05T00:00:00Z",
      "release_tag":"mib-processing-absdiff-laplacian-v0.1.0",
      "release_url":"https://example/releases/tag/mib-processing-absdiff-laplacian-v0.1.0",
      "native_plugins":[)") + c2Plugin + "]}";
    const auto c2Parsed = cat::parseVersionManifest(c2Manifest);
    MIB_REQUIRE(c2Parsed.ok, c2Parsed.error.toStdString());
    MIB_EXPECT(c2Parsed.version.line == "absdiff-laplacian" && c2Parsed.version.contractVersion == 2 &&
                   c2Parsed.version.releaseTag == "mib-processing-absdiff-laplacian-v0.1.0",
               "Contract-2 manifest takes its release identity from the top level (no wheel)");
    const auto* c2Native = cat::findNativePlugin(c2Parsed.version, "linux", "x86_64");
    MIB_REQUIRE(c2Native != nullptr, "Contract-2 Linux core listed");
    MIB_EXPECT(c2Native->engineAbiVersion == 2 && c2Native->algorithm == "absdiff-laplacian" &&
                   c2Native->entrypoint == "mib_processing_get_api_v2",
               "Contract-2 core metadata parsed");

    const QByteArray c2Index = QByteArray(R"({"processing_core_index_schema_version":1,
      "channel":"stable","line":"absdiff-laplacian","active_version":"0.1.0","versions":[
       {"version":"0.1.0","line":"absdiff-laplacian","contract_version":2,
        "published_at":"2026-10-05T00:00:00Z",
        "release_tag":"mib-processing-absdiff-laplacian-v0.1.0",
        "release_url":"https://example/releases/tag/mib-processing-absdiff-laplacian-v0.1.0",
        "manifest_url":"https://updates.example/stable/processing-core-absdiff-laplacian/versions/0.1.0.json",
        "native_plugins":[)") + c2Plugin + "]}]}";
    const auto c2IndexParsed = cat::parseIndex(c2Index);
    MIB_REQUIRE(c2IndexParsed.ok, c2IndexParsed.error.toStdString());
    MIB_EXPECT(c2IndexParsed.line == "absdiff-laplacian", "Contract-2 index line parsed");
    const auto c2Active = cat::validateCanonicalActive(c2IndexParsed, c2Parsed);
    MIB_EXPECT(c2Active.ok && c2Active.version == "0.1.0", "Contract-2 latest pointer validated");
    MIB_EXPECT(!cat::validateCanonicalActive(parsed, c2Parsed).ok,
               "a Contract-2 latest pointer is refused against a subtract-ring index");

    QByteArray c2WithV1Entry = c2Manifest;
    c2WithV1Entry.replace("\"entrypoint\":\"mib_processing_get_api_v2\"",
                          "\"entrypoint\":\"mib_processing_get_api\"");
    MIB_EXPECT(!cat::parseVersionManifest(c2WithV1Entry).ok,
               "an ABI-v2 core must use the v2 entry point");
    QByteArray c2AsContract1 = c2Manifest;
    c2AsContract1.replace("\"version\":\"0.1.0\",\"contract_version\":2",
                          "\"version\":\"0.1.0\",\"contract_version\":1");
    MIB_EXPECT(!cat::parseVersionManifest(c2AsContract1).ok,
               "an absdiff-laplacian manifest must declare Contract 2");
    QByteArray c2NoTag = c2Manifest;
    c2NoTag.replace("\"release_tag\":\"mib-processing-absdiff-laplacian-v0.1.0\",", "");
    MIB_EXPECT(!cat::parseVersionManifest(c2NoTag).ok,
               "a wheel-less manifest needs a top-level release tag");
    QByteArray unknownLine = c2Manifest;
    unknownLine.replace("\"line\":\"absdiff-laplacian\"", "\"line\":\"unet-cells\"");
    MIB_EXPECT(!cat::parseVersionManifest(unknownLine).ok, "unknown core line refused");
    QByteArray c2InC1Manifest = manifest;
    c2InC1Manifest.replace("\"entrypoint\":\"mib_processing_get_api\"",
                           "\"entrypoint\":\"mib_processing_get_api_v2\"");
    MIB_EXPECT(!cat::parseVersionManifest(c2InC1Manifest).ok,
               "a subtract-ring core with the v2 entry point is refused");
    QByteArray mixedIndex = c2Index;
    mixedIndex.replace("\"version\":\"0.1.0\",\"line\":\"absdiff-laplacian\"",
                       "\"version\":\"0.1.0\",\"line\":\"subtract-ring\"");
    MIB_EXPECT(!cat::parseIndex(mixedIndex).ok,
               "an index entry of another core line is refused");

    return mib::test::exitCode();
}
