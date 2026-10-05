#pragma once

// Qt shell's HTTPS POST for the central profile registry (#398, ADR 0002). The
// backend links no HTTP client; AppBackend::setProfileRegistryTransport() takes
// this QtNetwork-backed transport. A future Tauri/Rust shell supplies its own.

#include "backend/profiles/SupabaseProfileRegistry.h" // RegistryHttpTransport

namespace mib::frontend {

// Blocking POST run on the registry worker thread (own event loop). Enforces
// the transport contract: request timeout, response-size cap, no redirects,
// platform TLS verification (never ignored), prompt abort when the request's
// `cancelled` predicate turns true. Headers and bodies are never logged.
backend::profiles::RegistryHttpTransport makeQtRegistryHttpTransport();

} // namespace mib::frontend
