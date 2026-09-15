#include "frontend/system/QtLogBridge.h"

#include "backend/services/CrashReporter.h"

#include <spdlog/spdlog.h>

#include <QString>
#include <QtGlobal>

#include <string>

namespace mib::frontend {

namespace {

// Moved verbatim from the backend CrashReporter (epic #246): forward each Qt
// log line to spdlog with a [Qt] prefix, and escalate critical/fatal to Sentry.
void qtMessageHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg) {
    const std::string m = msg.toStdString();
    const char* file = ctx.file ? ctx.file : "";
    const char* fn   = ctx.function ? ctx.function : "";

    switch (type) {
        case QtDebugMsg:    SPDLOG_DEBUG("[Qt] {} ({}:{} {})", m, file, ctx.line, fn); break;
        case QtInfoMsg:     SPDLOG_INFO ("[Qt] {} ({}:{} {})", m, file, ctx.line, fn); break;
        case QtWarningMsg:  SPDLOG_WARN ("[Qt] {} ({}:{} {})", m, file, ctx.line, fn); break;
        case QtCriticalMsg: SPDLOG_ERROR("[Qt] {} ({}:{} {})", m, file, ctx.line, fn);
                             backend::services::CrashReporter::captureMessage("Qt critical: " + m); break;
        case QtFatalMsg:    SPDLOG_CRITICAL("[Qt FATAL] {} ({}:{} {})", m, file, ctx.line, fn);
                             backend::services::CrashReporter::captureMessage("Qt fatal: " + m); break;
    }
}

} // namespace

void installQtLogBridge() {
    qInstallMessageHandler(qtMessageHandler);
}

} // namespace mib::frontend
