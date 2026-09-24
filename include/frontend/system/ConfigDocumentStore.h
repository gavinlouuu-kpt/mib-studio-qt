// Checked document write helper shared by the config editors (issue #361)
// and the processing apply/persist path (issue #364).
//
// Writes go through QSaveFile (temporary file + atomic rename), the full
// write and commit() are checked, and the result carries the fingerprint of
// what landed on disk. A stale baseline (the file changed since it was
// loaded) is detected *before* writing and reported as a conflict instead of
// silently overwriting. This is not a cross-process compare-and-swap: two
// writers racing between the check and the rename can still interleave; the
// caller must treat a conflict result as "reconcile", not as "retry blindly".
#pragma once

#include <QByteArray>
#include <QString>

#include <optional>

namespace frontend {

struct ConfigWriteResult {
    bool ok{false};
    bool conflict{false};      // on-disk content differs from the expected baseline
    QString error;             // human-readable failure
    QByteArray fingerprint;    // sha256 of the content on disk after success
    qint64 bytesWritten{0};
};

class ConfigDocumentStore {
public:
    static QByteArray fingerprintOf(const QByteArray& bytes);
    static QByteArray fingerprintOf(const QString& text) { return fingerprintOf(text.toUtf8()); }

    // Fingerprint of the current file; nullopt when the file cannot be read
    // (missing file counts as "no baseline" => empty fingerprint).
    static std::optional<QByteArray> currentFingerprint(const QString& path);

    // Write `text` to `path`. When `expectedFingerprint` is given and differs
    // from the file's current fingerprint, nothing is written and the result
    // reports a conflict (unless `force`). Creates the parent directory.
    static ConfigWriteResult writeText(const QString& path, const QString& text,
                                       const std::optional<QByteArray>& expectedFingerprint = std::nullopt,
                                       bool force = false);
};

} // namespace frontend
