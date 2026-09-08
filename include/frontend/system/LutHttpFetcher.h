#pragma once

// Qt frontend's HTTP GET for the E-modulus LUT catalog (ADR 0002). The backend
// links no Qt networking; the shell injects this QtNetwork-backed fetcher via
// AppBackend::setLutHttpFetcher(). A future Tauri/Rust shell supplies its own.

#include "backend/processing/EModulusLutCatalog.h" // backend::HttpGetFn

namespace mib::frontend {

// A blocking HTTP GET (single-shot event loop + transfer timeout), matching the
// behaviour the backend catalog previously used directly.
backend::HttpGetFn makeQtLutHttpGet();

} // namespace mib::frontend
