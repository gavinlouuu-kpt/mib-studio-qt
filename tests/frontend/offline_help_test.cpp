#include <QApplication>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTextDocument>
#include <QPointer>
#include <QUrl>
#include <optional>
#include "frontend/utils/UpdateCatalog.h"
#define private public
#include "frontend/system/AutoUpdater.h"
#include "frontend/dialogs/SoftwareUpdatesDialog.h"
#undef private
#include "frontend/dialogs/HelpDialog.h"
#include "support/assert.h"
#include <QListWidget>
#include <QTimer>

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QCoreApplication::setApplicationVersion("9.8.7");
    QTemporaryDir dir;
    const auto write = [&](const QString& name, const QByteArray& text) {
        QFile file(dir.filePath(name));
        MIB_REQUIRE(file.open(QIODevice::WriteOnly), "fixture opens");
        file.write(text);
    };
    write("v9.8.7.md", "# Running release\n\nOffline notes (#573)");
    write("v9.8.6.md", "# Previous release");
    write("v10.0.0.md", "# Future release");
    frontend::HelpDialog notes(false, nullptr, dir.path());
    const QString rendered = notes.findChild<QTextBrowser*>()->toPlainText();
    MIB_EXPECT(rendered.startsWith("Running release"), "running release first");
    MIB_EXPECT(rendered.contains("Previous release") && !rendered.contains("Future release"),
               "earlier notes only");
    QImage image(2, 2, QImage::Format_RGB32);
    image.fill(Qt::blue);
    MIB_REQUIRE(image.save(dir.filePath("image.png")), "image fixture saved");
    write("index.md", "# Manual index\n\n![Offline image](image.png)\n\n[Connect](connect.md)");
    write("connect.md", "# Connect offline\n\nCamera instructions");
    frontend::HelpDialog manual(true, nullptr, dir.path());
    auto* browser = manual.findChild<QTextBrowser*>();
    MIB_EXPECT(browser->toPlainText().contains("Manual index"), "manual rendered");
    MIB_EXPECT(
        !browser->document()->resource(QTextDocument::ImageResource, QUrl("image.png")).isNull(),
        "local manual image resolves");
    browser->anchorClicked(QUrl("connect.md"));
    MIB_EXPECT(browser->toPlainText().contains("Camera instructions"), "local markdown link works");
    browser->anchorClicked(QUrl("#connect-offline"));
    MIB_EXPECT(browser->document()->baseUrl().toLocalFile().endsWith("connect.md"),
               "fragment links retain the current page");
    MIB_EXPECT(!frontend::shouldShowWhatsNew("", "9.8.7"), "fresh install stays quiet");
    MIB_EXPECT(frontend::shouldShowWhatsNew("9.8.6", "9.8.7"), "upgrade shows notes");
    MIB_EXPECT(!frontend::shouldShowWhatsNew("9.8.7", "9.8.7"), "once per version");
    MIB_EXPECT(frontend::shouldShowWhatsNew("9.8.7-beta.1", "9.8.7"),
               "beta to stable upgrade shows notes");
    MIB_EXPECT(!frontend::shouldShowWhatsNew("9.8.7", "9.8.6"), "downgrade stays quiet");
    frontend::AutoUpdater updater(nullptr);
    QString error;
    const auto manifest = updater.parseManifest(
        R"json({"version":"9.8.7","installer_url":"https://example.invalid/update.exe","installer_sha256":"aa","release_notes":"## Highlights\nOffline Help (#573)"})json",
        &error);
    MIB_REQUIRE(manifest.has_value(), "latest.json parses with inline notes");
    MIB_EXPECT(manifest->releaseNotes.contains("Offline Help"), "latest notes retained");
    QTimer::singleShot(0, []() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        MIB_REQUIRE(dialog, "update prompt opens");
        auto* notes = dialog->findChild<QTextBrowser*>();
        MIB_EXPECT(notes && notes->isVisible() && notes->toPlainText().contains("Offline Help"),
                   "prompt shows latest notes");
        dialog->reject();
    });
    MIB_EXPECT(!updater.confirmUpdate(*manifest), "closing prompt does not install");
    // No updater passed: this view test performs no network requests.
    frontend::SoftwareUpdatesDialog updates(nullptr);
    frontend::updatecatalog::VersionEntry entry;
    entry.version = "9.8.8";
    entry.releaseNotes = manifest->releaseNotes;
    updates.onIndexReady({entry});
    updates.findChild<QListWidget*>()->setCurrentRow(0);
    MIB_EXPECT(updates.findChild<QTextBrowser*>()->toPlainText().contains("Offline Help"),
               "selected update notes render");
    return mib::test::exitCode();
}
