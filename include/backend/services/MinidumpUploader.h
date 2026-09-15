#pragma once

// Direct minidump upload to Sentry's minidump endpoint with an HTTP status
// per dump. sentry-native's own transport gives no per-envelope delivery
// signal (0.7.20: sentry_capture_minidump returns void, a failed send is
// freed silently, a queued envelope survives a clean close only as a
// one-shot re-send on the next launch) — so the crash queue on disk is
// advanced only on a 2xx from this uploader. See CrashReporter.

#include <cstdint>
#include <filesystem>
#include <string>

namespace backend::services::crash_upload {

struct Dsn {
    std::string scheme;     // "https" | "http"
    std::string host;
    std::uint16_t port{0};  // 0 = scheme default
    std::string publicKey;
    std::string projectId;
    std::string basePath;   // "" or "/prefix" for DSNs with a path prefix
    bool valid{false};
};

// "scheme://key[:secret]@host[:port][/prefix]/projectId"
Dsn parseDsn(const std::string& dsn);

// "<scheme>://<host>[:port]<basePath>/api/<projectId>/minidump/?sentry_key=<key>"
std::string minidumpUrl(const Dsn& dsn);

struct UploadFields {
    std::string release;
    std::string environment;
    std::string buildId;            // exe PDB GUID+age (symbol key), may be empty
    std::string dumpFileName;       // original crash file name (tag)
    std::string crashMessage;       // terminate-path what(), may be empty
    std::string stateSnapshotJson;  // CrashStateMirror sidecar, may be empty
};

struct MultipartBody {
    std::string contentType;  // "multipart/form-data; boundary=..."
    std::string body;
};

// The request body the endpoint expects: a `sentry` JSON field (release,
// environment, tags, extra), the dump as `upload_file_minidump`, and the
// state sidecar as an attachment named state_snapshot.json.
MultipartBody buildMultipart(const std::string& dumpBytes, const UploadFields& fields,
                             const std::string& boundary);

struct UploadResult {
    bool transportOk{false};  // a response was received
    int httpStatus{0};        // valid when transportOk
    std::string error;        // transport error text when !transportOk
    bool delivered() const { return transportOk && httpStatus >= 200 && httpStatus < 300; }
    // 4xx other than timeout/rate-limit: the server will never take this
    // dump (too large, malformed, unknown project) — do not retry forever.
    bool rejected() const {
        return transportOk && httpStatus >= 400 && httpStatus < 500 && httpStatus != 408 &&
               httpStatus != 429;
    }
};

// Reads the dump and POSTs it. timeoutMs bounds the whole exchange.
UploadResult postMinidump(const Dsn& dsn, const std::filesystem::path& dump,
                          const UploadFields& fields, int timeoutMs);

// Raw POST used by postMinidump (exposed for the unit test).
UploadResult postMultipart(const Dsn& dsn, const std::string& url, const MultipartBody& body,
                           int timeoutMs);

// True when this build has an HTTP client for postMultipart (WinHTTP on
// Windows, libcurl elsewhere when available at build time).
bool httpClientAvailable();

} // namespace backend::services::crash_upload
