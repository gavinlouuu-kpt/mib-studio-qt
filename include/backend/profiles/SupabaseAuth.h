#pragma once
// Supabase Auth (GoTrue) password sign-in, refresh-token rotation and sign-out
// for the central profile registry (#398 M1). Same injected transport and
// endpoint rules as SupabaseProfileRegistry; worker-confined.
//
// Tokens live only in the returned AuthSession (process memory). Nothing here
// persists, logs or echoes a password or token; error messages never include
// response bodies.
#include "backend/profiles/SupabaseProfileRegistry.h"

#include <chrono>

namespace backend::profiles {

struct AuthSession {
    std::string accessToken;
    std::string refreshToken;
    std::string userId; // auth.users.id: the cache/subject identity
    std::string email;
    std::chrono::system_clock::time_point expiresAt{};
    bool valid() const { return !accessToken.empty() && !userId.empty(); }
};

class SupabaseAuth {
public:
    SupabaseAuth(std::string origin, std::string publishableKey, RegistryHttpTransport transport);
    void setCancellation(std::function<bool()> cancelled) { cancelled_ = std::move(cancelled); }

    // Authentication on rejected credentials, Offline on transport/5xx/429.
    AuthSession signInWithPassword(const std::string& email, const std::string& password);
    // Supabase rotates refresh tokens: the returned session replaces the old one.
    AuthSession refresh(const std::string& refreshToken);
    // Best effort server-side revocation; the caller discards tokens regardless.
    void signOut(const std::string& accessToken);

private:
    std::string post(const std::string& path, const std::string& body,
                     const std::string& bearer = {});
    AuthSession decodeSession(const std::string& body) const;

    std::string origin_;
    std::string publishableKey_;
    RegistryHttpTransport transport_;
    std::function<bool()> cancelled_;
};

} // namespace backend::profiles
