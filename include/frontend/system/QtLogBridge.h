#pragma once

// Routes Qt's process-wide log stream into spdlog (and Sentry for
// critical/fatal) via qInstallMessageHandler. Lives in the frontend because the
// backend links no Qt (epic #246); the handler previously sat in the backend's
// CrashReporter. Call once from main() after CrashReporter::init().

namespace mib::frontend {

void installQtLogBridge();

} // namespace mib::frontend
