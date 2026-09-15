#include "frontend/system/DesktopInstance.h"

#include <QDir>
#include <QFileInfo>

namespace frontend {

DesktopInstance::DesktopInstance(const QString& lockPath) : lock_(lockPath) {
    QDir().mkpath(QFileInfo(lockPath).absolutePath());
    // Long experiments are not stale. QLockFile still recovers a dead owner's
    // lock; never remove a live owner's lock just because enough time elapsed.
    lock_.setStaleLockTime(0);
}

bool DesktopInstance::acquire() {
    return lock_.tryLock(0);
}

QString DesktopInstance::failureMessage() const {
    qint64 pid = 0;
    QString host, name;
    if (lock_.getLockInfo(&pid, &host, &name)) {
        return QStringLiteral(
                   "MIB Studio is already running (process %1).\n\n"
                   "Use the existing window. If you just closed it, wait for it to finish "
                   "shutting down before reopening. If no window is visible, check that "
                   "process in Task Manager.\n\n"
                   "This launch has not opened any hardware.")
            .arg(pid);
    }
    return QStringLiteral("MIB Studio could not reserve its desktop session.\n\n"
                          "Check that your application-data folder is writable and that another "
                          "instance is not starting. This launch has not opened any hardware.");
}

} // namespace frontend
