#pragma once

#include <QString>
#include <QStringList>

namespace frontend::hdfreviewexport {

QString sourceBaseName(const QString& hdfPath);
QString metricsCsvPath(const QString& hdfPath, const QString& exportDir);
QString fcsPath(const QString& hdfPath, const QString& exportDir);
QString exportAllDirectoryPath(const QString& hdfPath, const QString& exportRootDir);
QStringList batchMetricsCsvPaths(const QStringList& hdfPaths, const QString& exportDir);
QStringList batchFcsPaths(const QStringList& hdfPaths, const QString& exportDir);
QStringList batchExportAllDirectoryPaths(const QStringList& hdfPaths, const QString& exportRootDir);

} // namespace frontend::hdfreviewexport
