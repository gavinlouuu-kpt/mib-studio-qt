#pragma once

#include <QLockFile>
#include <QString>

namespace frontend {

// Own before constructing the backend; destroy after all hardware is released.
// Use one per-user path across installed/development builds and update channels.
class DesktopInstance {
public:
    explicit DesktopInstance(const QString& lockPath);
    bool acquire();
    QString failureMessage() const;

private:
    QLockFile lock_;
};

} // namespace frontend
