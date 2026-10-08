#include "frontend/dialogs/SoftwareUpdatesDialog.h"

#include "frontend/system/AutoUpdater.h"

#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>

namespace frontend {

SoftwareUpdatesDialog::SoftwareUpdatesDialog(AutoUpdater* updater, QWidget* parent)
    : QDialog(parent), updater_(updater)
{
    setWindowTitle(tr("Software Updates"));
    resize(460, 420);

    auto* root = new QVBoxLayout(this);

    // Channel row.
    auto* channelRow = new QHBoxLayout();
    channelRow->addWidget(new QLabel(tr("Channel:"), this));
    channelBox_ = new QComboBox(this);
    channelBox_->addItem(tr("Stable"), QStringLiteral("stable"));
    channelBox_->addItem(tr("Beta"), QStringLiteral("beta"));
    if (updater_) {
        const int idx = channelBox_->findData(updater_->channel());
        if (idx >= 0) channelBox_->setCurrentIndex(idx);
    }
    channelRow->addWidget(channelBox_, 1);
    root->addLayout(channelRow);

    root->addWidget(new QLabel(tr("Available versions:"), this));
    list_ = new QListWidget(this);
    root->addWidget(list_, 1);

    notes_ = new QTextBrowser(this);
    notes_->setOpenExternalLinks(true);
    root->addWidget(notes_, 1);
    status_ = new QLabel(this);
    status_->setWordWrap(true);
    root->addWidget(status_);

    // Action buttons.
    auto* btnRow = new QHBoxLayout();
    installBtn_ = new QPushButton(tr("Install Selected"), this);
    notesBtn_ = new QPushButton(tr("Release Notes"), this);
    auto* checkBtn = new QPushButton(tr("Check for Latest"), this);
    auto* closeBtn = new QPushButton(tr("Close"), this);
    btnRow->addWidget(installBtn_);
    btnRow->addWidget(notesBtn_);
    btnRow->addWidget(checkBtn);
    btnRow->addStretch(1);
    btnRow->addWidget(closeBtn);
    root->addLayout(btnRow);

    connect(channelBox_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) {
                if (updater_) updater_->setChannel(channelBox_->currentData().toString());
                reload();
            });
    connect(list_, &QListWidget::itemSelectionChanged, this, &SoftwareUpdatesDialog::updateButtons);
    connect(installBtn_, &QPushButton::clicked, this, &SoftwareUpdatesDialog::installSelected);
    connect(notesBtn_, &QPushButton::clicked, this, &SoftwareUpdatesDialog::openSelectedNotes);
    connect(checkBtn, &QPushButton::clicked, this, [this]() {
        if (updater_) updater_->checkForUpdates(true);
    });
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);

    if (updater_) {
        connect(updater_, &AutoUpdater::versionIndexReady, this, &SoftwareUpdatesDialog::onIndexReady);
        connect(updater_, &AutoUpdater::versionIndexFailed, this, &SoftwareUpdatesDialog::onIndexFailed);
    }

    reload();
}

void SoftwareUpdatesDialog::reload()
{
    list_->clear();
    entries_.clear();
    status_->setText(tr("Loading version history…"));
    updateButtons();
    if (updater_) updater_->fetchVersionIndex();
}

void SoftwareUpdatesDialog::onIndexReady(const QVector<updatecatalog::VersionEntry>& versions)
{
    entries_ = versions;
    list_->clear();
    const QString current = updater_ ? updater_->currentVersion() : QString();

    for (const auto& e : entries_) {
        QString text = e.version;
        if (!e.publishedUtc.isEmpty()) text += QStringLiteral("   (%1)").arg(e.publishedUtc);
        if (!current.isEmpty() && e.version == current) text += tr("  — current");
        auto* item = new QListWidgetItem(text, list_);
        if (!current.isEmpty() && e.version == current) {
            // The installed version is shown but not installable.
            item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
            QFont f = item->font();
            f.setBold(true);
            item->setFont(f);
        }
    }

    if (entries_.isEmpty()) {
        status_->setText(tr("No versions are listed for this channel."));
    } else {
        status_->setText(tr("%n version(s) available.", "", static_cast<int>(entries_.size())));
    }
    updateButtons();
}

void SoftwareUpdatesDialog::onIndexFailed(const QString& error)
{
    list_->clear();
    entries_.clear();
    status_->setText(tr("Version history unavailable — use “Check for Latest”.\n%1").arg(error));
    updateButtons();
}

int SoftwareUpdatesDialog::selectedEntryIndex() const
{
    const int row = list_->currentRow();
    if (row < 0 || row >= entries_.size()) return -1;
    // currentRow tracks the model row, which matches entries_ ordering.
    if (!list_->item(row)->isSelected()) return -1;
    return row;
}

void SoftwareUpdatesDialog::updateButtons()
{
    const int idx = selectedEntryIndex();
    const QString current = updater_ ? updater_->currentVersion() : QString();
    const bool haveSel = idx >= 0;
    notes_->setMarkdown(haveSel ? entries_[idx].releaseNotes : QString());
    notes_->setVisible(haveSel && !entries_[idx].releaseNotes.isEmpty());
    installBtn_->setEnabled(haveSel && entries_[idx].version != current);
    notesBtn_->setEnabled(haveSel && !entries_[idx].releaseNotesUrl.isEmpty());
}

void SoftwareUpdatesDialog::installSelected()
{
    const int idx = selectedEntryIndex();
    if (idx < 0 || !updater_) return;
    const auto& entry = entries_[idx];
    const QString current = updater_->currentVersion();
    if (entry.version == current) return;

    if (updatecatalog::isDowngrade(entry.version, current)) {
        const auto choice = QMessageBox::warning(
            this, tr("Confirm downgrade"),
            tr("Version %1 is older than the installed version %2.\n\n"
               "Install it anyway?")
                .arg(entry.version, current),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (choice != QMessageBox::Yes) return;
    }

    updater_->installVersion(entry);
    accept();
}

void SoftwareUpdatesDialog::openSelectedNotes()
{
    const int idx = selectedEntryIndex();
    if (idx < 0) return;
    const QString url = entries_[idx].releaseNotesUrl;
    if (!url.isEmpty()) QDesktopServices::openUrl(QUrl(url));
}

} // namespace frontend
