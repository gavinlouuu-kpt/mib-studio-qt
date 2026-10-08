#include "backend/app/ProfileCatalog.h"
#include <nlohmann/json.hpp>
#include <string>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#elif defined(MIB_HAVE_CURL) && MIB_HAVE_CURL
#include <curl/curl.h>
#endif
namespace backend::app {
namespace {
constexpr size_t limit = 4 * 1024 * 1024;
#if defined(_WIN32)
struct Handle {
    HINTERNET value{};
    ~Handle() {
        if (value) WinHttpCloseHandle(value);
    }
};
std::wstring wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()),
                                nullptr, 0);
    if (!n) throw std::runtime_error("Invalid URL encoding");
    std::wstring out(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}
#elif defined(MIB_HAVE_CURL) && MIB_HAVE_CURL
size_t receive(char* bytes, size_t size, size_t count, void* context) {
    auto& out = *static_cast<std::string*>(context);
    if (size && count > (limit - out.size()) / size) return 0;
    out.append(bytes, size * count);
    return size * count;
}
#endif
} // namespace
std::string fetchProfileUrl(const std::string& url) {
    nlohmann::json result = {{"ok", false}};
    try {
        if (url.size() > 4096 || (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0))
            throw std::runtime_error("Catalog URLs must use HTTP(S)");
        const auto authorityStart = url.find("://") + 3;
        const auto authorityEnd = url.find('/', authorityStart);
        if (url.substr(authorityStart, authorityEnd == std::string::npos
                                           ? std::string::npos
                                           : authorityEnd - authorityStart)
                .find('@') != std::string::npos)
            throw std::runtime_error("Catalog URL credentials are not supported");
        std::string body;
#ifdef _WIN32
        auto u = wide(url);
        URL_COMPONENTS c{};
        c.dwStructSize = sizeof(c);
        c.dwHostNameLength = static_cast<DWORD>(-1);
        c.dwUrlPathLength = static_cast<DWORD>(-1);
        c.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(u.c_str(), static_cast<DWORD>(u.size()), 0, &c))
            throw std::runtime_error("Invalid catalog URL");
        Handle session{WinHttpOpen(L"MIB-Studio-Profiles/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
        if (!session.value) throw std::runtime_error("Cannot initialize profile transport");
        WinHttpSetTimeouts(session.value, 15000, 15000, 15000, 15000);
        const std::wstring host(c.lpszHostName, c.dwHostNameLength);
        Handle connection{WinHttpConnect(session.value, host.c_str(), c.nPort, 0)};
        if (!connection.value) throw std::runtime_error("Cannot connect profile host");
        std::wstring path(c.lpszUrlPath, c.dwUrlPathLength);
        if (c.dwExtraInfoLength) path.append(c.lpszExtraInfo, c.dwExtraInfoLength);
        Handle request{
            WinHttpOpenRequest(connection.value, L"GET", path.empty() ? L"/" : path.c_str(),
                               nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                               c.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)};
        if (!request.value) throw std::runtime_error("Cannot open profile request");
        DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirects,
                         sizeof(redirects));
        if (!WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request.value, nullptr))
            throw std::runtime_error("Profile download failed");
        DWORD status = 0, bytes = sizeof(status);
        if (!WinHttpQueryHeaders(
                request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &bytes, WINHTTP_NO_HEADER_INDEX) ||
            status < 200 || status >= 300)
            throw std::runtime_error("Profile server did not return success");
        char chunk[16384];
        for (;;) {
            DWORD got = 0;
            if (!WinHttpReadData(request.value, chunk, sizeof(chunk), &got))
                throw std::runtime_error("Profile response read failed");
            if (!got) break;
            if (body.size() + got > limit)
                throw std::runtime_error("Profile response exceeds 4 MiB");
            body.append(chunk, got);
        }
#elif defined(MIB_HAVE_CURL) && MIB_HAVE_CURL
        CURL* curl = curl_easy_init();
        if (!curl) throw std::runtime_error("Cannot initialize profile transport");
        struct Cleanup {
            CURL* p;
            ~Cleanup() { curl_easy_cleanup(p); }
        } cleanup{curl};
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 15000L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
#if LIBCURL_VERSION_NUM >= 0x075500
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
#else
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "MIB-Studio-Profiles/1");
        const auto code = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (code != CURLE_OK)
            throw std::runtime_error(std::string("Profile download failed: ") +
                                     curl_easy_strerror(code));
        if (status < 200 || status >= 300)
            throw std::runtime_error("Profile server did not return success");
#else
        throw std::runtime_error("This build has no HTTP profile transport");
#endif
        (void)nlohmann::json(body).dump();
        result["body"] = body;
        result["ok"] = true;
    } catch (const std::exception& e) {
        result["error"] = e.what();
    }
    return result.dump();
}
} // namespace backend::app
