#include "frontend/system/ConfigDocumentStore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace frontend {

QByteArray ConfigDocumentStore::fingerprintOf(const QByteArray& bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

std::optional<QByteArray> ConfigDocumentStore::currentFingerprint(const QString& path)
{
    QFile f(path);
    if (!f.exists()) return QByteArray();
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    return fingerprintOf(f.readAll());
}

ConfigWriteResult ConfigDocumentStore::writeText(const QString& path, const QString& text,
                                                 const std::optional<QByteArray>& expectedFingerprint, bool force)
{
    ConfigWriteResult r;
    if (path.trimmed().isEmpty()) {
        r.error = QStringLiteral("no document path");
        return r;
    }
    const QFileInfo info(path);
    if (info.exists() && info.isDir()) {
        r.error = QStringLiteral("path is a directory: %1").arg(path);
        return r;
    }
    if (expectedFingerprint && !force) {
        const auto onDisk = currentFingerprint(path);
        if (!onDisk) {
            r.error = QStringLiteral("cannot read the existing document to verify it is unchanged: %1").arg(path);
            return r;
        }
        if (*onDisk != *expectedFingerprint) {
            r.conflict = true;
            r.error = QStringLiteral("the document changed on disk since it was loaded: %1").arg(path);
            return r;
        }
    }
    QDir dir = info.absoluteDir();
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        r.error = QStringLiteral("cannot create directory: %1").arg(dir.absolutePath());
        return r;
    }
    const QByteArray bytes = text.toUtf8();
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        r.error = QStringLiteral("cannot open for writing: %1 (%2)").arg(path, file.errorString());
        return r;
    }
    const qint64 written = file.write(bytes);
    if (written != bytes.size()) {
        r.error = QStringLiteral("short write (%1 of %2 bytes): %3").arg(written).arg(bytes.size()).arg(path);
        file.cancelWriting();
        return r;
    }
    if (!file.commit()) {
        r.error = QStringLiteral("commit failed: %1 (%2)").arg(path, file.errorString());
        return r;
    }
    r.ok = true;
    r.bytesWritten = written;
    r.fingerprint = fingerprintOf(bytes);
    return r;
}

} // namespace frontend
