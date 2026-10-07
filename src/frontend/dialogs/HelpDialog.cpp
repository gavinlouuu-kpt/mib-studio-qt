#include "frontend/dialogs/HelpDialog.h"
#include "frontend/utils/UpdateCatalog.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QPushButton>
#include <QTextBrowser>
#include <QTextDocument>
#include <QUrl>
#include <QVBoxLayout>
#include <QVersionNumber>

#include <algorithm>

namespace frontend {
namespace {
QString readMarkdown(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}
} // namespace

QString helpContentDirectory(const QString& name) {
    const QString installed = QCoreApplication::applicationDirPath() + "/resources/" + name;
    if (QDir(installed).exists()) return installed;
    return QStringLiteral(MIB_HELP_SOURCE_DIR) + "/docs/" + name;
}

bool shouldShowWhatsNew(const QString& previous, const QString& current) {
    return !previous.isEmpty() && previous != current &&
           !updatecatalog::isDowngrade(current, previous);
}

QString bundledReleaseNotes(const QString& directory, const QString& version) {
    QStringList files = QDir(directory).entryList({"v*.md"}, QDir::Files);
    const auto numeric = [](const QString& file) {
        return QVersionNumber::fromString(file.mid(1));
    };
    std::sort(files.begin(), files.end(), [&](const QString& a, const QString& b) {
        if (a == "v" + version + ".md") return b != a;
        if (b == "v" + version + ".md") return false;
        return numeric(a) > numeric(b);
    });
    QStringList notes;
    for (const QString& file : files) {
        if (file != "v" + version + ".md" && numeric(file) >= QVersionNumber::fromString(version))
            continue;
        notes.append(readMarkdown(QDir(directory).filePath(file)));
    }
    return notes.isEmpty()
               ? QStringLiteral("# What's New\n\nNo release notes are bundled for this version.")
               : notes.join("\n\n---\n\n");
}

HelpDialog::HelpDialog(bool manual, QWidget* parent, const QString& directory) : QDialog(parent) {
    setWindowTitle(manual ? tr("User Manual") : tr("What's New"));
    resize(800, 650);
    auto* layout = new QVBoxLayout(this);
    auto* browser = new QTextBrowser(this);
    browser->setObjectName("helpBrowser");
    browser->setOpenLinks(false);
    layout->addWidget(browser);
    const QString root =
        directory.isEmpty() ? helpContentDirectory(manual ? "manual" : "release-notes") : directory;
    const auto load = [browser, root](const QUrl& url) {
        const QString path = url.toLocalFile();
        const QString canonical = QFileInfo(path).canonicalFilePath();
        if (!canonical.startsWith(QDir(root).canonicalPath() + "/") || !path.endsWith(".md"))
            return;
        browser->document()->setBaseUrl(QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()));
        browser->setMarkdown(readMarkdown(path));
        if (!url.fragment().isEmpty()) browser->scrollToAnchor(url.fragment());
    };
    if (manual) {
        load(QUrl::fromLocalFile(QDir(root).absoluteFilePath("index.md")));
        connect(browser, &QTextBrowser::anchorClicked, this, [browser, load](const QUrl& url) {
            const QUrl resolved = browser->document()->baseUrl().resolved(url);
            if (resolved.isLocalFile())
                load(resolved);
            else if (resolved.scheme() == "https" || resolved.scheme() == "http")
                QDesktopServices::openUrl(resolved);
        });
    } else {
        browser->setMarkdown(bundledReleaseNotes(root, QCoreApplication::applicationVersion()));
        connect(browser, &QTextBrowser::anchorClicked, this, [](const QUrl& url) {
            if (url.scheme() == "https" || url.scheme() == "http") QDesktopServices::openUrl(url);
        });
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    if (manual) {
        auto* index = buttons->addButton(tr("Index"), QDialogButtonBox::ActionRole);
        connect(index, &QPushButton::clicked, this, [load, root]() {
            load(QUrl::fromLocalFile(QDir(root).absoluteFilePath("index.md")));
        });
        auto* online =
            buttons->addButton(tr("Open online documentation"), QDialogButtonBox::ActionRole);
        connect(online, &QPushButton::clicked, this, []() {
            QDesktopServices::openUrl(QUrl("https://kpt1020.github.io/mib-studio-qt/"));
        });
    }
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

} // namespace frontend
