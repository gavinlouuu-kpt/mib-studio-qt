#include "backend/profiles/SupabaseAuth.h"

#include <nlohmann/json.hpp>

namespace backend::profiles {
namespace {
using Json = nlohmann::json;
constexpr size_t kMaxAuthResponseBytes = 64 * 1024;
} // namespace

SupabaseAuth::SupabaseAuth(std::string origin, std::string key, RegistryHttpTransport transport)
    : origin_(std::move(origin)), publishableKey_(std::move(key)),
      transport_(std::move(transport)) {
    validateSupabaseEndpoint(origin_, publishableKey_);
    if (!transport_) throw RegistryError(RegistryErrorCode::Invalid, "Auth transport required");
}

std::string SupabaseAuth::post(const std::string& path, const std::string& body,
                               const std::string& bearer) {
    if (bearer.find_first_of("\r\n") != std::string::npos)
        throw RegistryError(RegistryErrorCode::Authentication, "Invalid session token");
    RegistryHttpRequest request{
        origin_ + path, body, {{"apikey", publishableKey_}, {"Content-Type", "application/json"}}};
    if (!bearer.empty()) request.headers["Authorization"] = "Bearer " + bearer;
    request.maxResponseBytes = kMaxAuthResponseBytes;
    request.cancelled = cancelled_;
    const auto response = transport_(request);
    if (response.status == 0 || response.status == 408 || response.status == 429 ||
        response.status >= 500)
        throw RegistryError(RegistryErrorCode::Offline, "Registry sign-in service unavailable");
    // GoTrue answers bad credentials / unknown or reused refresh tokens with
    // 400 (invalid_grant / invalid_credentials), expired JWTs with 401/403.
    if (response.status == 400 || response.status == 401 || response.status == 403 ||
        response.status == 422)
        throw RegistryError(RegistryErrorCode::Authentication, "Registry sign-in rejected");
    if (response.status < 200 || response.status >= 300)
        throw RegistryError(RegistryErrorCode::Invalid, "Registry sign-in request failed");
    if (response.body.size() > kMaxAuthResponseBytes)
        throw RegistryError(RegistryErrorCode::Invalid, "Registry sign-in response exceeds limit");
    return response.body;
}

AuthSession SupabaseAuth::decodeSession(const std::string& body) const {
    try {
        const auto j = Json::parse(body);
        AuthSession s;
        s.accessToken = j.at("access_token").get<std::string>();
        s.refreshToken = j.at("refresh_token").get<std::string>();
        const auto& user = j.at("user");
        s.userId = user.at("id").get<std::string>();
        if (user.contains("email") && user.at("email").is_string())
            s.email = user.at("email").get<std::string>();
        const auto expiresIn = j.at("expires_in").get<int64_t>();
        if (s.accessToken.empty() || s.refreshToken.empty() || s.userId.empty() || expiresIn <= 0)
            throw RegistryError(RegistryErrorCode::Invalid, "Incomplete registry session");
        s.expiresAt = std::chrono::system_clock::now() + std::chrono::seconds(expiresIn);
        return s;
    } catch (const Json::exception&) {
        throw RegistryError(RegistryErrorCode::Invalid, "Malformed registry session");
    }
}

AuthSession SupabaseAuth::signInWithPassword(const std::string& email,
                                             const std::string& password) {
    if (email.empty() || password.empty())
        throw RegistryError(RegistryErrorCode::Authentication, "Email and password required");
    return decodeSession(post("/auth/v1/token?grant_type=password",
                              Json({{"email", email}, {"password", password}}).dump()));
}

AuthSession SupabaseAuth::refresh(const std::string& refreshToken) {
    if (refreshToken.empty())
        throw RegistryError(RegistryErrorCode::Authentication, "Sign-in required");
    return decodeSession(post("/auth/v1/token?grant_type=refresh_token",
                              Json({{"refresh_token", refreshToken}}).dump()));
}

void SupabaseAuth::signOut(const std::string& accessToken) {
    if (!accessToken.empty()) post("/auth/v1/logout", "{}", accessToken);
}

} // namespace backend::profiles
