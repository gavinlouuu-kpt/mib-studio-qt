#include "backend/services/MinidumpUploader.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <winhttp.h>
#  pragma comment(lib, "winhttp.lib")
#elif defined(MIB_HAVE_CURL) && MIB_HAVE_CURL
#  include <curl/curl.h>
#endif

namespace backend::services::crash_upload {

namespace {

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

void appendPart(std::string& body, const std::string& boundary, const std::string& name,
                const std::string& filename, const std::string& contentType,
                const std::string& data) {
    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"" + name + "\"";
    if (!filename.empty()) body += "; filename=\"" + filename + "\"";
    body += "\r\n";
    if (!contentType.empty()) body += "Content-Type: " + contentType + "\r\n";
    body += "\r\n";
    body += data;
    body += "\r\n";
}

} // namespace

Dsn parseDsn(const std::string& text) {
    Dsn d;
    const auto schemeEnd = text.find("://");
    if (schemeEnd == std::string::npos) return d;
    d.scheme = text.substr(0, schemeEnd);
    if (d.scheme != "https" && d.scheme != "http") return d;
    const auto at = text.find('@', schemeEnd + 3);
    if (at == std::string::npos) return d;
    std::string auth = text.substr(schemeEnd + 3, at - (schemeEnd + 3));
    const auto colon = auth.find(':');
    d.publicKey = colon == std::string::npos ? auth : auth.substr(0, colon);
    if (d.publicKey.empty()) return d;
    const auto pathStart = text.find('/', at + 1);
    if (pathStart == std::string::npos) return d;
    std::string hostPort = text.substr(at + 1, pathStart - (at + 1));
    const auto portSep = hostPort.rfind(':');
    if (portSep != std::string::npos) {
        d.host = hostPort.substr(0, portSep);
        const long p = std::strtol(hostPort.c_str() + portSep + 1, nullptr, 10);
        if (p <= 0 || p > 65535) return d;
        d.port = static_cast<std::uint16_t>(p);
    } else {
        d.host = hostPort;
    }
    if (d.host.empty()) return d;
    std::string path = text.substr(pathStart);  // "/[prefix/]projectId"
    while (!path.empty() && path.back() == '/') path.pop_back();
    const auto lastSlash = path.rfind('/');
    if (lastSlash == std::string::npos) return d;
    d.projectId = path.substr(lastSlash + 1);
    d.basePath = path.substr(0, lastSlash);
    if (d.projectId.empty()) return d;
    d.valid = true;
    return d;
}

std::string minidumpUrl(const Dsn& dsn) {
    std::string url = dsn.scheme + "://" + dsn.host;
    if (dsn.port != 0) url += ":" + std::to_string(dsn.port);
    url += dsn.basePath + "/api/" + dsn.projectId + "/minidump/?sentry_key=" + dsn.publicKey;
    return url;
}

MultipartBody buildMultipart(const std::string& dumpBytes, const UploadFields& fields,
                             const std::string& boundary) {
    std::ostringstream sentry;
    sentry << "{\"release\":\"" << jsonEscape(fields.release) << "\""
           << ",\"environment\":\"" << jsonEscape(fields.environment) << "\""
           << ",\"tags\":{\"crash_recovery\":\"pending_dump\""
           << ",\"original_dump_file\":\"" << jsonEscape(fields.dumpFileName) << "\"";
    if (!fields.buildId.empty()) {
        sentry << ",\"exe_build_id\":\"" << jsonEscape(fields.buildId) << "\"";
    }
    sentry << "}";
    if (!fields.crashMessage.empty()) {
        sentry << ",\"extra\":{\"crash_message\":\"" << jsonEscape(fields.crashMessage) << "\"}";
    }
    sentry << "}";

    MultipartBody out;
    out.contentType = "multipart/form-data; boundary=" + boundary;
    appendPart(out.body, boundary, "sentry", "", "", sentry.str());
    appendPart(out.body, boundary, "upload_file_minidump", fields.dumpFileName,
               "application/octet-stream", dumpBytes);
    if (!fields.stateSnapshotJson.empty()) {
        appendPart(out.body, boundary, "state_snapshot", "state_snapshot.json",
                   "application/json", fields.stateSnapshotJson);
    }
    out.body += "--" + boundary + "--\r\n";
    return out;
}

bool httpClientAvailable() {
#if defined(_WIN32) || (defined(MIB_HAVE_CURL) && MIB_HAVE_CURL)
    return true;
#else
    return false;
#endif
}

#ifdef _WIN32
namespace {
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

struct HInternet {
    HINTERNET h{nullptr};
    ~HInternet() { if (h) ::WinHttpCloseHandle(h); }
};
} // namespace

UploadResult postMultipart(const Dsn& dsn, const std::string& url, const MultipartBody& body,
                           int timeoutMs) {
    UploadResult r;
    HInternet session;
    session.h = ::WinHttpOpen(L"mib-studio-qt-crash-upload/1",
                              WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) {
        session.h = ::WinHttpOpen(L"mib-studio-qt-crash-upload/1",
                                  WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!session.h) {
        r.error = "WinHttpOpen failed: " + std::to_string(::GetLastError());
        return r;
    }
    ::WinHttpSetTimeouts(session.h, 10000, 10000, timeoutMs, timeoutMs);

    const bool secure = dsn.scheme == "https";
    const INTERNET_PORT port =
        dsn.port != 0 ? dsn.port : (secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT);
    HInternet connect;
    connect.h = ::WinHttpConnect(session.h, widen(dsn.host).c_str(), port, 0);
    if (!connect.h) {
        r.error = "WinHttpConnect failed: " + std::to_string(::GetLastError());
        return r;
    }

    // Path + query: everything after "scheme://host[:port]".
    const auto hostStart = url.find("://") + 3;
    const auto pathStart = url.find('/', hostStart);
    const std::string pathAndQuery = pathStart == std::string::npos ? "/" : url.substr(pathStart);
    HInternet request;
    request.h = ::WinHttpOpenRequest(connect.h, L"POST", widen(pathAndQuery).c_str(), nullptr,
                                     WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request.h) {
        r.error = "WinHttpOpenRequest failed: " + std::to_string(::GetLastError());
        return r;
    }
    const std::wstring headers = widen("Content-Type: " + body.contentType + "\r\n");
    if (!::WinHttpSendRequest(request.h, headers.c_str(), static_cast<DWORD>(-1),
                              const_cast<char*>(body.body.data()),
                              static_cast<DWORD>(body.body.size()),
                              static_cast<DWORD>(body.body.size()), 0)) {
        r.error = "WinHttpSendRequest failed: " + std::to_string(::GetLastError());
        return r;
    }
    if (!::WinHttpReceiveResponse(request.h, nullptr)) {
        r.error = "WinHttpReceiveResponse failed: " + std::to_string(::GetLastError());
        return r;
    }
    DWORD status = 0;
    DWORD size = sizeof(status);
    if (!::WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                               WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                               WINHTTP_NO_HEADER_INDEX)) {
        r.error = "WinHttpQueryHeaders failed: " + std::to_string(::GetLastError());
        return r;
    }
    r.transportOk = true;
    r.httpStatus = static_cast<int>(status);
    return r;
}
#elif defined(MIB_HAVE_CURL) && MIB_HAVE_CURL
UploadResult postMultipart(const Dsn& /*dsn*/, const std::string& url, const MultipartBody& body,
                           int timeoutMs) {
    UploadResult r;
    CURL* curl = curl_easy_init();
    if (!curl) {
        r.error = "curl_easy_init failed";
        return r;
    }
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("Content-Type: " + body.contentType).c_str());
    headers = curl_slist_append(headers, "Expect:");
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.body.size()));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeoutMs));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "mib-studio-qt-crash-upload/1");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    // Discard the response body.
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                     +[](char*, size_t s, size_t n, void*) -> size_t { return s * n; });
    const CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        r.error = curl_easy_strerror(rc);
    } else {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        r.transportOk = true;
        r.httpStatus = static_cast<int>(status);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return r;
}
#else
UploadResult postMultipart(const Dsn&, const std::string&, const MultipartBody&, int) {
    UploadResult r;
    r.error = "no HTTP client in this build";
    return r;
}
#endif

UploadResult postMinidump(const Dsn& dsn, const std::filesystem::path& dump,
                          const UploadFields& fields, int timeoutMs) {
    UploadResult r;
    std::ifstream in(dump, std::ios::binary);
    if (!in) {
        r.error = "cannot read " + dump.string();
        return r;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string boundary =
        "----mib-crash-" + std::to_string(std::hash<std::string>{}(dump.string())) + "-" +
        std::to_string(static_cast<unsigned long long>(ss.str().size()));
    const MultipartBody body = buildMultipart(ss.str(), fields, boundary);
    return postMultipart(dsn, minidumpUrl(dsn), body, timeoutMs);
}

} // namespace backend::services::crash_upload
