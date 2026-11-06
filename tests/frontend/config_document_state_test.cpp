// config_document_state_test (issue #361)
//
// Pure editor-document state + checked document store: dirty is a content
// comparison (not label visibility), an external change while dirty is a
// conflict that retains local bytes, a checked QSaveFile write replaces the
// file atomically, a stale baseline is refused as a conflict before writing,
// a failed write keeps the state dirty, and a file-as-directory target fails
// cleanly. readDocument() returns the raw bytes of a CRLF file and its
// fingerprint equals the on-disk one (issue #904).

#include "frontend/models/ConfigDocumentState.h"
#include "frontend/system/ConfigDocumentStore.h"

#include "support/assert.h"
#include "support/tempdir.h"

#include <QDir>
#include <QFile>

int main()
{
    using frontend::ConfigDocumentState;
    using frontend::ConfigDocumentStore;
    mib::test::TempDir td("config_document_state");
    const QString path = QString::fromStdString((td.path() / "config.json").string());

    // ---- state ------------------------------------------------------------
    ConfigDocumentState doc;
    doc.markLoaded(path, QStringLiteral("{\"a\":1}"));
    MIB_EXPECT(!doc.dirty && !doc.conflict && doc.stateLabel() == QStringLiteral("Loaded"), "clean after load");
    doc.markEdited(QStringLiteral("{\"a\":2}"));
    MIB_EXPECT(doc.dirty && doc.stateLabel() == QStringLiteral("Edited"), "edit -> dirty");
    doc.markEdited(QStringLiteral("{\"a\":1}"));
    MIB_EXPECT(!doc.dirty, "editing back to the baseline is clean (content comparison)");
    MIB_EXPECT(doc.markExternalChange(), "external change while clean may reload");
    doc.markEdited(QStringLiteral("{\"a\":3}"));
    MIB_EXPECT(!doc.markExternalChange() && doc.conflict && doc.dirty, "external change while dirty -> conflict, edits retained");
    MIB_EXPECT(doc.stateLabel() == QStringLiteral("Conflict"), "conflict label");
    doc.markSaveFailed(QStringLiteral("disk full"));
    MIB_EXPECT(doc.dirty && doc.lastSave == ConfigDocumentState::SaveOutcome::Failed, "failed save keeps dirty");
    doc.markSaved(QStringLiteral("{\"a\":3}"));
    MIB_EXPECT(!doc.dirty && !doc.conflict && doc.stateLabel() == QStringLiteral("Saved"), "saved -> clean");

    // ---- store --------------------------------------------------------------
    {
        const auto r = ConfigDocumentStore::writeText(path, QStringLiteral("{\"a\":1}\n"));
        MIB_REQUIRE(r.ok && !r.conflict && r.bytesWritten == 8, "initial write");
        MIB_EXPECT(ConfigDocumentStore::currentFingerprint(path).value_or(QByteArray()) == r.fingerprint, "fingerprint matches disk");
        // Stale baseline: someone else wrote the file.
        {
            QFile f(path);
            MIB_REQUIRE(f.open(QIODevice::WriteOnly | QIODevice::Truncate), "external writer");
            f.write("{\"a\":99}\n");
        }
        const auto stale = ConfigDocumentStore::writeText(path, QStringLiteral("{\"a\":2}\n"), r.fingerprint);
        MIB_EXPECT(!stale.ok && stale.conflict, "stale baseline refused as conflict");
        QFile check(path);
        check.open(QIODevice::ReadOnly);
        MIB_EXPECT(check.readAll() == QByteArray("{\"a\":99}\n"), "conflicting write did not touch the file");
        check.close();
        const auto forced = ConfigDocumentStore::writeText(path, QStringLiteral("{\"a\":2}\n"), r.fingerprint, /*force=*/true);
        MIB_EXPECT(forced.ok, "forced overwrite succeeds");
        const auto matching = ConfigDocumentStore::writeText(path, QStringLiteral("{\"a\":3}\n"), forced.fingerprint);
        MIB_EXPECT(matching.ok, "matching baseline writes");
        // Missing file with an empty expected fingerprint = "no baseline".
        const QString fresh = QString::fromStdString((td.path() / "sub" / "new.json").string());
        const auto created = ConfigDocumentStore::writeText(fresh, QStringLiteral("x"), QByteArray());
        MIB_EXPECT(created.ok && QFile::exists(fresh), "creates parent directory + file");
        // Directory target fails cleanly.
        const QString dirPath = QString::fromStdString((td.path() / "adir").string());
        QDir().mkpath(dirPath);
        const auto bad = ConfigDocumentStore::writeText(dirPath, QStringLiteral("x"));
        MIB_EXPECT(!bad.ok && !bad.conflict && bad.error.contains(QStringLiteral("directory")), "directory target fails");
        // File as parent fails cleanly.
        const auto bad2 = ConfigDocumentStore::writeText(path + QStringLiteral("/child.json"), QStringLiteral("x"));
        MIB_EXPECT(!bad2.ok, "file-as-parent fails");
        MIB_EXPECT(ConfigDocumentStore::writeText(QString(), QStringLiteral("x")).ok == false, "empty path fails");
    }

    // ---- raw-bytes read (issue #904) ----------------------------------------
    // The one read behind every fingerprint keeps CRLF, so "what was loaded"
    // and "what is on disk" hash the same bytes on every platform.
    {
        const QString crlfPath = QString::fromStdString((td.path() / "crlf.json").string());
        const QByteArray crlf("{\r\n  \"a\": 1,\r\n  \"b\": [1, 2]\r\n}\r\n");
        {
            QFile f(crlfPath);
            MIB_REQUIRE(f.open(QIODevice::WriteOnly | QIODevice::Truncate), "write CRLF fixture");
            f.write(crlf);
        }
        const auto rr = ConfigDocumentStore::readDocument(crlfPath);
        MIB_EXPECT(rr.ok && rr.exists && rr.bytes == crlf, "readDocument returns the raw bytes, CRLF kept");
        MIB_EXPECT(rr.fingerprint == ConfigDocumentStore::fingerprintOf(crlf)
                       && ConfigDocumentStore::currentFingerprint(crlfPath).value_or(QByteArray()) == rr.fingerprint,
                   "loaded fingerprint == on-disk fingerprint for a CRLF file");
        // Why: a QIODevice::Text read drops the '\r', so its fingerprint can
        // never match the on-disk one (the #904 Apply refusal).
        {
            QFile f(crlfPath);
            MIB_REQUIRE(f.open(QIODevice::ReadOnly | QIODevice::Text), "text-mode read");
            const QByteArray text = f.readAll();
            MIB_EXPECT(!text.contains('\r') && ConfigDocumentStore::fingerprintOf(text) != rr.fingerprint,
                       "a Text-mode read of a CRLF file does not fingerprint the on-disk bytes");
        }
        // A save of the app's own text writes exactly those bytes ('\n' stays '\n').
        const auto w = ConfigDocumentStore::writeText(crlfPath, QStringLiteral("{\n  \"a\": 2\n}\n"), rr.fingerprint);
        const auto after = ConfigDocumentStore::readDocument(crlfPath);
        MIB_EXPECT(w.ok && after.bytes == QByteArray("{\n  \"a\": 2\n}\n") && after.fingerprint == w.fingerprint,
                   "write fingerprint is that of the bytes on disk");
        const auto missing = ConfigDocumentStore::readDocument(crlfPath + QStringLiteral(".nope"));
        MIB_EXPECT(!missing.ok && !missing.exists && !missing.error.isEmpty(), "missing file: not ok, not exists");
        MIB_EXPECT(ConfigDocumentStore::currentFingerprint(crlfPath + QStringLiteral(".nope")).value_or(QByteArray("x")).isEmpty(),
                   "missing file still counts as an empty baseline");
    }
    return mib::test::exitCode();
}
