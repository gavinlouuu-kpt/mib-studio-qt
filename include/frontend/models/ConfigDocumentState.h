// Explicit per-document editor state (issue #361).
//
// The config/profile editors used to infer "unsaved" from a label's
// visibility, which stops being true the moment a parent tab or inspector is
// hidden. This value type is the single source of truth for one edited
// document (app config.json, camera script, MindVision JSON): which path is
// active, the fingerprint of the content that was loaded/saved, whether the
// current editor content differs from it, whether the file changed
// elsewhere while local edits exist, and the outcome of the last checked
// save. Labels render this state; visibility never controls it.
#pragma once

#include <QByteArray>
#include <QCryptographicHash>
#include <QString>

namespace frontend {

struct ConfigDocumentState {
    enum class SaveOutcome { None, Saved, Failed, Conflict };

    QString path;
    QByteArray loadedFingerprint;   // sha256 of the loaded/saved content
    QByteArray currentFingerprint;  // sha256 of the editor content
    bool dirty{false};              // editor content != loaded content
    bool conflict{false};           // file changed elsewhere while dirty
    SaveOutcome lastSave{SaveOutcome::None};
    QString lastError;

    static QByteArray fingerprint(const QString& content)
    {
        return QCryptographicHash::hash(content.toUtf8(), QCryptographicHash::Sha256);
    }

    // A file was (re)loaded into the editor: clean baseline.
    void markLoaded(const QString& documentPath, const QString& content)
    {
        path = documentPath;
        loadedFingerprint = fingerprint(content);
        currentFingerprint = loadedFingerprint;
        dirty = false;
        conflict = false;
        lastSave = SaveOutcome::None;
        lastError.clear();
    }

    // The editor content changed (user edit or table rebuild). Dirty is a
    // content comparison, so undoing an edit back to the baseline is clean.
    void markEdited(const QString& content)
    {
        currentFingerprint = fingerprint(content);
        dirty = currentFingerprint != loadedFingerprint;
        if (!dirty && conflict) {
            // Edits gone: a pending external change can be reloaded by the
            // owner; keep the conflict flag until it does.
        }
    }

    // The watched file changed on disk. Returns true when the owner may
    // reload it (no local edits); otherwise the conflict is recorded and the
    // local bytes must be retained.
    bool markExternalChange()
    {
        if (dirty) {
            conflict = true;
            return false;
        }
        return true;
    }

    void markSaved(const QString& content)
    {
        loadedFingerprint = fingerprint(content);
        currentFingerprint = loadedFingerprint;
        dirty = false;
        conflict = false;
        lastSave = SaveOutcome::Saved;
        lastError.clear();
    }

    void markSaveFailed(const QString& error, bool becauseOfConflict = false)
    {
        lastSave = becauseOfConflict ? SaveOutcome::Conflict : SaveOutcome::Failed;
        lastError = error;
        if (becauseOfConflict) conflict = true;
        // dirty stays true: nothing was persisted.
    }

    // Short, distinct state token for compact labels.
    QString stateLabel() const
    {
        if (conflict) return QStringLiteral("Conflict");
        if (dirty) return QStringLiteral("Edited");
        if (lastSave == SaveOutcome::Saved) return QStringLiteral("Saved");
        return QStringLiteral("Loaded");
    }
};

} // namespace frontend
