#include "frontend/utils/UpdateCatalog.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QVersionNumber>

#include <algorithm>
#include <climits>

namespace frontend::updatecatalog {
namespace {

// "1.0.4-beta.2" -> core {1,0,4}. A bare release also yields its core.
QVersionNumber coreOf(const QString& v)
{
    const int dash = v.indexOf('-');
    return QVersionNumber::fromString(dash < 0 ? v : v.left(dash));
}

// Beta ordinal: a release sorts AFTER its betas, so it gets INT_MAX.
int betaOf(const QString& v)
{
    const int idx = v.indexOf(QStringLiteral("-beta."));
    if (idx < 0) return INT_MAX;
    bool ok = false;
    const int n = v.mid(idx + 6).toInt(&ok);
    return ok ? n : 0;
}

// True if a is a newer version than b.
bool isNewer(const QString& a, const QString& b)
{
    const QVersionNumber ca = coreOf(a), cb = coreOf(b);
    if (ca != cb) return ca > cb;
    return betaOf(a) > betaOf(b);
}

} // namespace

ParseResult parseIndex(const QByteArray& bytes)
{
    ParseResult r;
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &perr);
    if (doc.isNull()) {
        r.error = QStringLiteral("JSON parse error: ") + perr.errorString();
        return r;
    }
    if (!doc.isObject()) {
        r.error = QStringLiteral("index root is not an object");
        return r;
    }

    const QJsonArray arr = doc.object().value(QStringLiteral("versions")).toArray();
    for (const auto& v : arr) {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        VersionEntry e;
        e.version = o.value(QStringLiteral("version")).toString().trimmed();
        e.installerUrl = o.value(QStringLiteral("installer_url")).toString().trimmed();
        e.installerSha256Hex = o.value(QStringLiteral("installer_sha256")).toString().trimmed().toLower();
        if (e.version.isEmpty() || e.installerUrl.isEmpty() || e.installerSha256Hex.isEmpty()) {
            continue; // skip malformed entry rather than fail the whole list
        }
        e.releaseNotesUrl = o.value(QStringLiteral("release_notes_url")).toString().trimmed();
        e.releaseNotes = o.value(QStringLiteral("release_notes")).toString().left(16384);
        e.publishedUtc = o.value(QStringLiteral("published_utc")).toString().trimmed();
        e.installerSizeBytes =
            static_cast<qint64>(o.value(QStringLiteral("installer_size_bytes")).toDouble(-1));
        r.versions.push_back(e);
    }

    std::stable_sort(r.versions.begin(), r.versions.end(),
                     [](const VersionEntry& a, const VersionEntry& b) {
                         return isNewer(a.version, b.version);
                     });
    r.ok = true;
    return r;
}

int indexOfVersion(const QVector<VersionEntry>& versions, const QString& current)
{
    for (int i = 0; i < versions.size(); ++i) {
        if (versions[i].version == current) return i;
    }
    return -1;
}

bool isDowngrade(const QString& candidate, const QString& current)
{
    if (candidate == current) return false;
    return !isNewer(candidate, current);
}

QString channelForVersion(const QString& version)
{
    return version.contains(QStringLiteral("-beta."))
               ? QStringLiteral("beta")
               : QStringLiteral("stable");
}

} // namespace frontend::updatecatalog
