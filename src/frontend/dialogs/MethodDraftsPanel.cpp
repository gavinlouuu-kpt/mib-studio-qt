#include "frontend/dialogs/MethodDraftsPanel.h"

#include "frontend/dialogs/CentralMethodsDialog.h"

#include "backend/profiles/ProfileRegistryWorker.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QVBoxLayout>

namespace frontend {

namespace {

using backend::profiles::MethodDraft;
using backend::profiles::RegistryWorkerSnapshot;

QString q(const std::string& s) {
    return QString::fromStdString(s);
}

const MethodDraft* findDraft(const RegistryWorkerSnapshot& s, const std::string& id) {
    for (const auto& d : s.drafts)
        if (d.draftId == id) return &d;
    return nullptr;
}

QString revisionLabel(const RegistryWorkerSnapshot& s, const std::string& id) {
    if (id.empty()) return {};
    for (const auto& r : s.revisions)
        if (r.revisionId == id) return QStringLiteral("r%1").arg(r.revisionNumber);
    return q(id.substr(0, 8)) + QStringLiteral("…");
}

QString changesText(const std::vector<std::string>& keys) {
    if (keys.empty()) return MethodDraftsPanel::tr("none");
    QStringList out;
    for (size_t i = 0; i < keys.size() && i < 12; ++i) out << q(keys[i]);
    if (keys.size() > 12) out << MethodDraftsPanel::tr("… %1 more").arg(static_cast<int>(keys.size() - 12));
    return out.join(QStringLiteral(", "));
}

} // namespace

MethodDraftsPanel::MethodDraftsPanel(backend::profiles::ProfileRegistryWorker& registry,
                                     const CentralMethodsHooks& hooks,
                                     std::function<void(const QString&, bool)> notice, QWidget* parent)
    : QWidget(parent), registry_(registry), hooks_(hooks), notice_(std::move(notice)) {
    auto* root = new QVBoxLayout(this);
    table_ = new QTableWidget(0, 5, this);
    table_->setObjectName(QStringLiteral("centralMethodsDrafts"));
    table_->setHorizontalHeaderLabels({tr("Method"), tr("Based on"), tr("Status"), tr("Release notes"),
                                       tr("Updated (UTC)")});
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->verticalHeader()->hide();
    table_->horizontalHeader()->setStretchLastSection(true);
    root->addWidget(table_, 1);

    conflict_ = new QLabel(this);
    conflict_->setObjectName(QStringLiteral("centralMethodsConflict"));
    conflict_->setWordWrap(true);
    conflict_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    conflict_->setStyleSheet(QStringLiteral("color: #8a4b00;"));
    conflict_->hide();
    root->addWidget(conflict_);

    auto* row = new QHBoxLayout();
    const auto button = [this, row](const QString& text, const char* name) {
        auto* b = new QPushButton(text, this);
        b->setObjectName(QString::fromLatin1(name));
        row->addWidget(b);
        return b;
    };
    newMethodBtn_ = button(tr("New method from current config..."), "centralMethodsNewMethod");
    notesBtn_ = button(tr("Release notes..."), "centralMethodsDraftNotes");
    submitBtn_ = button(tr("Submit for review"), "centralMethodsSubmit");
    branchBtn_ = button(tr("Submit as branch..."), "centralMethodsSubmitBranch");
    fromHeadBtn_ = button(tr("New draft from head..."), "centralMethodsDraftFromHead");
    discardBtn_ = button(tr("Discard..."), "centralMethodsDiscardDraft");
    row->addStretch(1);
    root->addLayout(row);

    auto* note = new QLabel(tr("Drafts stay on this PC under your registry account until you submit them. "
                               "A submitted revision is immutable and needs an independent review before "
                               "it can be published."),
                            this);
    note->setWordWrap(true);
    root->addWidget(note);

    connect(newMethodBtn_, &QPushButton::clicked, this, &MethodDraftsPanel::newMethod);
    connect(notesBtn_, &QPushButton::clicked, this, &MethodDraftsPanel::editNotes);
    connect(submitBtn_, &QPushButton::clicked, this, [this] { submit(false); });
    connect(branchBtn_, &QPushButton::clicked, this, [this] { submit(true); });
    connect(fromHeadBtn_, &QPushButton::clicked, this, &MethodDraftsPanel::draftFromHead);
    connect(discardBtn_, &QPushButton::clicked, this, &MethodDraftsPanel::discard);
    connect(table_, &QTableWidget::itemSelectionChanged, this, [this] { updateActions(registry_.snapshot()); });
}

QString MethodDraftsPanel::selectedDraft() const {
    const auto rows = table_->selectionModel() ? table_->selectionModel()->selectedRows() : QModelIndexList{};
    if (rows.isEmpty()) return {};
    const auto* item = table_->item(rows.first().row(), 0);
    return item ? item->data(Qt::UserRole).toString() : QString{};
}

void MethodDraftsPanel::render(const RegistryWorkerSnapshot& s) {
    const QString keep = selectedDraft();
    const QSignalBlocker block(table_);
    table_->setRowCount(static_cast<int>(s.drafts.size()));
    for (int row = 0; row < static_cast<int>(s.drafts.size()); ++row) {
        const auto& d = s.drafts[static_cast<size_t>(row)];
        QString status;
        if (!d.submittedRevisionId.empty())
            status = tr("Submitted as %1").arg(revisionLabel(s, d.submittedRevisionId));
        else if (s.submitConflict && s.submitConflict->draftId == d.draftId)
            status = tr("CONFLICT: head is now %1").arg(revisionLabel(s, s.submitConflict->headRevisionId));
        else
            status = tr("Draft (not submitted)");
        const QStringList cells{q(d.methodDisplayName),
                                d.newMethod ? tr("new method") : revisionLabel(s, d.baseRevisionId), status,
                                q(d.releaseNotes).section(QLatin1Char('\n'), 0, 0), q(d.updatedAtUtc)};
        for (int col = 0; col < cells.size(); ++col) {
            auto* item = new QTableWidgetItem(cells[col]);
            item->setData(Qt::UserRole, q(d.draftId));
            if (col == 3) item->setToolTip(q(d.releaseNotes));
            table_->setItem(row, col, item);
        }
        if (q(d.draftId) == keep) table_->selectRow(row);
    }
    table_->resizeColumnsToContents();
    updateActions(s);
}

void MethodDraftsPanel::updateActions(const RegistryWorkerSnapshot& s) {
    const auto* d = findDraft(s, selectedDraft().toStdString());
    const bool hasCache = s.configured && s.session != RegistryWorkerSnapshot::Session::SignedOut;
    const bool signedIn = s.session == RegistryWorkerSnapshot::Session::SignedIn;
    const bool idle = hasCache && !s.busy;
    const bool open = d && d->submittedRevisionId.empty();
    const bool author = d && backend::profiles::hasProjectRole(s, d->projectId, "author");
    const bool conflict = d && s.submitConflict && s.submitConflict->draftId == d->draftId;
    bool anyAuthorProject = false;
    for (const auto& p : s.projects) anyAuthorProject |= backend::profiles::hasProjectRole(s, p.projectId, "author");

    newMethodBtn_->setEnabled(idle && anyAuthorProject && hooks_.currentConfigDraft);
    newMethodBtn_->setToolTip(anyAuthorProject ? tr("A new central method from the applied config.json")
                                               : tr("Needs the author role in a project"));
    notesBtn_->setEnabled(idle && open);
    submitBtn_->setEnabled(idle && signedIn && open && author && !conflict);
    submitBtn_->setToolTip(!signedIn ? tr("Sign in to submit")
                           : d && !author ? tr("Needs the author role in this project")
                                          : tr("Send this draft as an immutable candidate revision"));
    branchBtn_->setEnabled(idle && signedIn && open && author && conflict);
    fromHeadBtn_->setEnabled(idle && open && conflict && !s.submitConflict->headRevisionId.empty());
    discardBtn_->setEnabled(idle && d);

    if (conflict) {
        const auto& c = *s.submitConflict;
        QString text = tr("This draft is based on %1, but %2 has been published since. Nothing was sent.")
                           .arg(revisionLabel(s, c.baseRevisionId), revisionLabel(s, c.headRevisionId));
        if (c.compared)
            text += QStringLiteral("\n") + tr("Changed upstream: %1").arg(changesText(c.upstreamChanges)) +
                    QStringLiteral("\n") + tr("Your draft differs from the head in: %1").arg(changesText(c.draftVsHead));
        else
            text += QStringLiteral("\n") + tr("Refresh to compare the two revisions.");
        text += QStringLiteral("\n") + tr("Choose: submit as a branch of %1, start a new draft from %2, or discard.")
                                           .arg(revisionLabel(s, c.baseRevisionId), revisionLabel(s, c.headRevisionId));
        conflict_->setText(text);
        conflict_->show();
    } else {
        conflict_->hide();
    }
}

void MethodDraftsPanel::newMethod() {
    const auto s = registry_.snapshot();
    QStringList names;
    std::vector<std::string> ids;
    for (const auto& p : s.projects)
        if (backend::profiles::hasProjectRole(s, p.projectId, "author")) {
            names << q(p.displayName);
            ids.push_back(p.projectId);
        }
    if (ids.empty() || !hooks_.currentConfigDraft) return;
    int index = 0;
    if (ids.size() > 1) {
        const auto chosen = hooks_.chooseItem(tr("New central method"), tr("Project:"), names);
        if (!chosen) return;
        index = static_cast<int>(names.indexOf(*chosen));
        if (index < 0) return;
    }
    const auto name = hooks_.askText(tr("New central method"), tr("Method name:"), {}, false);
    if (!name || name->trimmed().isEmpty()) return;
    const auto notes = hooks_.askText(tr("New central method"), tr("Release notes for the first revision:"), {}, true);
    if (!notes) return;
    std::string error;
    auto draft = hooks_.currentConfigDraft(&error);
    if (!error.empty()) {
        notice_(tr("Cannot start a draft: %1").arg(q(error)), true);
        return;
    }
    draft.projectId = ids[static_cast<size_t>(index)];
    draft.newMethod = true;
    draft.methodDisplayName = name->trimmed().toStdString();
    draft.releaseNotes = notes->toStdString();
    if (draft.hardwareCompatibilityJson.empty()) draft.hardwareCompatibilityJson = "{}";
    if (registry_.requestSaveDraft(std::move(draft)) == 0) {
        notice_(tr("The draft was refused."), true);
        return;
    }
    notice_(tr("Saving the draft... (it stays local until you submit it)"), false);
}

void MethodDraftsPanel::editNotes() {
    const auto s = registry_.snapshot();
    const auto* d = findDraft(s, selectedDraft().toStdString());
    if (!d) return;
    const auto notes = hooks_.askText(tr("Release notes"), tr("Release notes for %1:").arg(q(d->methodDisplayName)),
                                      q(d->releaseNotes), true);
    if (!notes) return;
    auto edited = *d;
    edited.releaseNotes = notes->toStdString();
    registry_.requestSaveDraft(std::move(edited));
}

void MethodDraftsPanel::submit(bool asBranch) {
    const auto s = registry_.snapshot();
    const auto* d = findDraft(s, selectedDraft().toStdString());
    if (!d) return;
    if (asBranch && !hooks_.confirm(tr("Submit as branch"),
                                    tr("Submit \"%1\" as a branch of %2? The current head stays published; "
                                       "this branch cannot be published until the lineage is resolved.")
                                        .arg(q(d->methodDisplayName), revisionLabel(s, d->baseRevisionId))))
        return;
    if (registry_.requestSubmitDraft(d->draftId, asBranch) == 0) return;
    notice_(asBranch ? tr("Submitting as a branch...") : tr("Submitting for review..."), false);
}

void MethodDraftsPanel::draftFromHead() {
    const auto s = registry_.snapshot();
    const auto* d = findDraft(s, selectedDraft().toStdString());
    if (!d || !s.submitConflict || s.submitConflict->draftId != d->draftId) return;
    const bool keepConfig = hooks_.confirm(
        tr("New draft from head"),
        tr("Start a new draft from %1 with your draft's config.json? (No: use %1's config.json.) "
           "Your current draft is kept until you discard it.")
            .arg(revisionLabel(s, s.submitConflict->headRevisionId)));
    MethodDraft next;
    next.methodDisplayName = d->methodDisplayName;
    next.releaseNotes = d->releaseNotes;
    if (keepConfig) next.configJson = d->configJson;
    registry_.requestSaveDraft(std::move(next), s.submitConflict->headRevisionId);
    notice_(tr("Creating a draft from the current head..."), false);
}

void MethodDraftsPanel::discard() {
    const auto s = registry_.snapshot();
    const auto* d = findDraft(s, selectedDraft().toStdString());
    if (!d) return;
    if (!hooks_.confirm(tr("Discard draft"), tr("Discard the local draft of \"%1\"? This cannot be undone.")
                                                 .arg(q(d->methodDisplayName))))
        return;
    registry_.requestDeleteDraft(d->draftId);
}

} // namespace frontend
