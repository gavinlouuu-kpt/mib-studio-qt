// Default folder for the Start Experiment save dialog (issue #906).
//
// A profile's `save_directory` is often relative ("testing"). Resolving it
// against the working directory put the dialog in the install folder (on an
// installed Windows build, C:\Program Files\MIB Studio Qt, which a normal
// user cannot write). A relative value resolves under the user's data root
// <Documents>/MIB_Studio_Qt (the folder Help > Open Data Folder opens); an
// absolute value is used as is; "" or a path that climbs out of the root
// gives the root itself.
#pragma once

#include <QDir>
#include <QString>

namespace frontend {

inline QString resolveExperimentSaveDirectory(const QString& saveDirectory, const QString& documentsDir)
{
    const QString base = documentsDir.trimmed().isEmpty() ? QDir::homePath() : documentsDir.trimmed();
    const QString root = QDir::cleanPath(QDir(base).absoluteFilePath(QStringLiteral("MIB_Studio_Qt")));
    // Profiles travel between platforms: a '\\' is a Windows separator.
    const QString configured = QString(saveDirectory.trimmed()).replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (configured.isEmpty()) return root;
    if (QDir::isAbsolutePath(configured)) return QDir::cleanPath(configured);
    const QString resolved = QDir::cleanPath(QDir(root).absoluteFilePath(configured));
    if (resolved != root && !resolved.startsWith(root + QLatin1Char('/'))) return root;
    return resolved;
}

} // namespace frontend
