// Pure parse + ordering for the per-channel update index.json. QtCore only, no
// I/O, so it can be unit tested without the app or network.
#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

namespace frontend::updatecatalog {

// One installable release. Field set mirrors what AutoUpdater's installer
// download path needs (url, sha256, size, notes) plus display/sort metadata.
struct VersionEntry {
    QString version;
    QString installerUrl;
    QString installerSha256Hex;
    QString releaseNotesUrl;
    QString releaseNotes;
    QString publishedUtc;
    qint64 installerSizeBytes{-1};
};

struct ParseResult {
    bool ok{false};
    QString error;                  // set when ok == false
    QVector<VersionEntry> versions; // newest-first
};

// Parse a channel index.json. Rejects malformed/non-object documents; skips
// individual entries missing version/installer_url/installer_sha256 rather than
// failing the whole list. Result is sorted newest-first (a release sorts above
// its own betas; betas ascend by number).
ParseResult parseIndex(const QByteArray& bytes);

// Index of `current` in `versions`, or -1 if absent.
int indexOfVersion(const QVector<VersionEntry>& versions, const QString& current);

// True if installing `candidate` over `current` is a downgrade (older, or a
// beta of an already-installed release). Equal versions are not a downgrade.
bool isDowngrade(const QString& candidate, const QString& current);

// The update channel a version string belongs to: "beta" if it carries a
// "-beta." pre-release suffix, otherwise "stable". Used so a build knows its own
// channel (the build version must retain its suffix for this to work).
QString channelForVersion(const QString& version);

} // namespace frontend::updatecatalog
