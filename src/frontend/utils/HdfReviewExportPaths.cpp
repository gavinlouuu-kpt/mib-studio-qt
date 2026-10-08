#include "frontend/utils/HdfReviewExportPaths.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>

namespace frontend::hdfreviewexport {
namespace {

QString reservationKey(const QString& path)
{
    QString clean = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
#ifdef Q_OS_WIN
    clean = clean.toCaseFolded();
#endif
    return clean;
}

bool isAvailable(const QString& path, const QSet<QString>& reserved)
{
    return !QFileInfo::exists(path) && !reserved.contains(reservationKey(path));
}

QString eventMapPath(const QString& fcsPath)
{
    const QFileInfo info(fcsPath);
    return QDir(info.absolutePath()).filePath(info.completeBaseName() + QStringLiteral("_event_map.csv"));
}

QString uniqueFcsPath(const QString& dirPath, const QString& firstStem, QSet<QString>& reserved)
{
    const QDir dir(dirPath);
    for (int suffix = 1; suffix < 1000000; ++suffix) {
        const QString stem = suffix == 1 ? firstStem : QStringLiteral("%1_%2").arg(firstStem).arg(suffix);
        const QString fcs = dir.filePath(stem + QStringLiteral(".fcs"));
        const QString map = eventMapPath(fcs);
        if (isAvailable(fcs, reserved) && isAvailable(map, reserved)) {
            reserved.insert(reservationKey(fcs));
            reserved.insert(reservationKey(map));
            return fcs;
        }
    }
    return {};
}

QString uniquePath(const QString& dirPath,
                   const QString& firstName,
                   const QString& numberedPattern,
                   QSet<QString>& reserved)
{
    const QDir dir(dirPath);
    QString candidate = dir.filePath(firstName);
    if (isAvailable(candidate, reserved)) {
        reserved.insert(reservationKey(candidate));
        return candidate;
    }

    for (int suffix = 2; suffix < 1000000; ++suffix) {
        candidate = dir.filePath(numberedPattern.arg(suffix));
        if (isAvailable(candidate, reserved)) {
            reserved.insert(reservationKey(candidate));
            return candidate;
        }
    }

    reserved.insert(reservationKey(candidate));
    return candidate;
}

} // namespace

QString sourceBaseName(const QString& hdfPath)
{
    const QFileInfo info(hdfPath);
    QString base = info.completeBaseName().trimmed();
    if (base.isEmpty()) {
        base = QStringLiteral("hdf_export");
    }
    return base;
}

QString metricsCsvPath(const QString& hdfPath, const QString& exportDir)
{
    QSet<QString> reserved;
    const QString base = sourceBaseName(hdfPath);
    return uniquePath(exportDir,
                      QStringLiteral("%1_metrics.csv").arg(base),
                      QStringLiteral("%1_metrics_%2.csv").arg(base),
                      reserved);
}

QString fcsPath(const QString& hdfPath, const QString& exportDir)
{
    QSet<QString> reserved;
    return uniqueFcsPath(exportDir, sourceBaseName(hdfPath), reserved);
}

QString exportAllDirectoryPath(const QString& hdfPath, const QString& exportRootDir)
{
    QSet<QString> reserved;
    const QString base = sourceBaseName(hdfPath);
    return uniquePath(exportRootDir,
                      base,
                      QStringLiteral("%1_%2").arg(base),
                      reserved);
}

QStringList batchMetricsCsvPaths(const QStringList& hdfPaths, const QString& exportDir)
{
    QSet<QString> reserved;
    QStringList results;
    results.reserve(hdfPaths.size());
    for (const QString& hdfPath : hdfPaths) {
        const QString base = sourceBaseName(hdfPath);
        results.push_back(uniquePath(exportDir,
                                     QStringLiteral("%1_metrics.csv").arg(base),
                                     QStringLiteral("%1_metrics_%2.csv").arg(base),
                                     reserved));
    }
    return results;
}

QStringList batchFcsPaths(const QStringList& hdfPaths, const QString& exportDir)
{
    QSet<QString> reserved;
    QStringList results;
    results.reserve(hdfPaths.size());
    for (const QString& hdfPath : hdfPaths)
        results.push_back(uniqueFcsPath(exportDir, sourceBaseName(hdfPath), reserved));
    return results;
}

QStringList batchExportAllDirectoryPaths(const QStringList& hdfPaths, const QString& exportRootDir)
{
    QSet<QString> reserved;
    QStringList results;
    results.reserve(hdfPaths.size());
    for (const QString& hdfPath : hdfPaths) {
        const QString base = sourceBaseName(hdfPath);
        results.push_back(uniquePath(exportRootDir,
                                     base,
                                     QStringLiteral("%1_%2").arg(base),
                                     reserved));
    }
    return results;
}

} // namespace frontend::hdfreviewexport
