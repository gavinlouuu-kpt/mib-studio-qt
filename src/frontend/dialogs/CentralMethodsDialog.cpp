#include "frontend/dialogs/CentralMethodsDialog.h"

#include "frontend/dialogs/MethodDraftsPanel.h"

#include "backend/app/MethodApply.h"
#include "backend/profiles/ProfileRegistryWorker.h"

#include <QDateTime>
#include <QFileDialog>
#include <QInputDialog>
#include <QPlainTextEdit>
#include <QTabWidget>
#include <QSignalBlocker>
#include <QMessageBox>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <chrono>

namespace frontend {

namespace {

using backend::profiles::CentralState;
using backend::profiles::RegistryHealth;
using backend::profiles::RegistryJobState;
using backend::profiles::RegistryWorkerSnapshot;

QString q(const std::string& s) {
    return QString::fromStdString(s);
}

QString connectivityText(const RegistryWorkerSnapshot& s) {
    if (!s.configured) {
        return CentralMethodsDialog::tr(
            "Central registry: not configured (set MIB_PROFILE_REGISTRY_URL and "
            "MIB_PROFILE_REGISTRY_PUBLISHABLE_KEY)");
    }
    switch (s.health.connectivity) {
    case RegistryHealth::Connectivity::Online:
        return CentralMethodsDialog::tr("Central registry: online");
    case RegistryHealth::Connectivity::Offline:
        return CentralMethodsDialog::tr(
            "Central registry: offline (cached methods stay available)");
    case RegistryHealth::Connectivity::AuthenticationRequired:
        return CentralMethodsDialog::tr("Central registry: sign-in required");
    case RegistryHealth::Connectivity::PermissionDenied:
        return CentralMethodsDialog::tr("Central registry: access denied");
    case RegistryHealth::Connectivity::Failed:
        return CentralMethodsDialog::tr("Central registry: request failed");
    case RegistryHealth::Connectivity::Unknown:
        break;
    }
    return CentralMethodsDialog::tr("Central registry: not contacted yet");
}

QString accountText(const RegistryWorkerSnapshot& s) {
    const QString who = s.email.empty() ? q(s.subjectId) : q(s.email);
    switch (s.session) {
    case RegistryWorkerSnapshot::Session::SignedIn:
        return CentralMethodsDialog::tr("Signed in as %1").arg(who);
    case RegistryWorkerSnapshot::Session::CachedOffline:
        return CentralMethodsDialog::tr("Cached methods for %1 (not signed in; sign in to refresh)")
            .arg(who);
    case RegistryWorkerSnapshot::Session::SignedOut:
        break;
    }
    return CentralMethodsDialog::tr("Not signed in");
}

QString stateText(CentralState state) {
    switch (state) {
    case CentralState::Submitted:
        return CentralMethodsDialog::tr("Submitted");
    case CentralState::Approved:
        return CentralMethodsDialog::tr("Approved (not published)");
    case CentralState::Rejected:
        return CentralMethodsDialog::tr("Rejected");
    case CentralState::Published:
        return CentralMethodsDialog::tr("Published");
    case CentralState::Superseded:
        return CentralMethodsDialog::tr("Superseded");
    case CentralState::Archived:
        return CentralMethodsDialog::tr("Archived");
    case CentralState::Revoked:
        return CentralMethodsDialog::tr("REVOKED - do not use");
    }
    return {};
}

QString jobText(const backend::profiles::RegistryJobStatus& job) {
    if (job.id == 0) return {};
    const QString kind = q(backend::profiles::toString(job.kind)).replace('_', ' ');
    const QString state = q(backend::profiles::toString(job.state));
    return job.message.empty() ? CentralMethodsDialog::tr("Last action: %1 %2").arg(kind, state)
                               : CentralMethodsDialog::tr("Last action: %1 %2 - %3")
                                     .arg(kind, state, q(job.message));
}

bool usableState(CentralState state) {
    return state == CentralState::Published || state == CentralState::Superseded;
}

const backend::profiles::CachedRevisionSummary* findRevision(const RegistryWorkerSnapshot& s,
                                                             const std::string& id) {
    for (const auto& r : s.revisions)
        if (r.revisionId == id) return &r;
    return nullptr;
}

constexpr int kChangesShown = 25;

} // namespace

CentralMethodsDialog::CentralMethodsDialog(backend::profiles::ProfileRegistryWorker& registry,
                                           CentralMethodsHooks hooks, QWidget* parent)
    : QDialog(parent), registry_(registry), hooks_(std::move(hooks)) {
    if (!hooks_.pickEvidenceFile)
        hooks_.pickEvidenceFile = [this](const QString& title) {
            return QFileDialog::getOpenFileName(this, title, {}, tr("HDF5 runs (*.h5 *.hdf5)"));
        };
    if (!hooks_.confirm)
        hooks_.confirm = [this](const QString& title, const QString& text) {
            return QMessageBox::question(this, title, text) == QMessageBox::Yes;
        };
    if (!hooks_.askText)
        hooks_.askText = [this](const QString& title, const QString& label, const QString& initial,
                                bool multiline) -> std::optional<QString> {
            bool ok = false;
            const QString text = multiline ? QInputDialog::getMultiLineText(this, title, label, initial, &ok)
                                           : QInputDialog::getText(this, title, label, QLineEdit::Normal, initial, &ok);
            return ok ? std::optional<QString>(text) : std::nullopt;
        };
    if (!hooks_.chooseItem)
        hooks_.chooseItem = [this](const QString& title, const QString& label,
                                   const QStringList& items) -> std::optional<QString> {
            bool ok = false;
            const QString item = QInputDialog::getItem(this, title, label, items, 0, false, &ok);
            return ok ? std::optional<QString>(item) : std::nullopt;
        };
    setWindowTitle(tr("Central Methods"));
    resize(960, 560);
    auto* root = new QVBoxLayout(this);

    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("centralMethodsStatus"));
    status_->setWordWrap(true);
    QFont bold = status_->font();
    bold.setBold(true);
    status_->setFont(bold);
    root->addWidget(status_);

    account_ = new QLabel(this);
    account_->setObjectName(QStringLiteral("centralMethodsAccount"));
    root->addWidget(account_);

    instrument_ = new QLabel(this);
    instrument_->setObjectName(QStringLiteral("centralMethodsInstrument"));
    root->addWidget(instrument_);

    activity_ = new QLabel(this);
    activity_->setObjectName(QStringLiteral("centralMethodsActivity"));
    activity_->setWordWrap(true);
    root->addWidget(activity_);

    warning_ = new QLabel(this);
    warning_->setObjectName(QStringLiteral("centralMethodsWarning"));
    warning_->setWordWrap(true);
    warning_->setStyleSheet(QStringLiteral("color: #b00020;"));
    warning_->hide();
    root->addWidget(warning_);

    signInRow_ = new QWidget(this);
    auto* signInLayout = new QHBoxLayout(signInRow_);
    signInLayout->setContentsMargins(0, 0, 0, 0);
    email_ = new QLineEdit(signInRow_);
    email_->setObjectName(QStringLiteral("centralMethodsEmail"));
    email_->setPlaceholderText(tr("Email"));
    password_ = new QLineEdit(signInRow_);
    password_->setObjectName(QStringLiteral("centralMethodsPassword"));
    password_->setPlaceholderText(tr("Password"));
    password_->setEchoMode(QLineEdit::Password);
    signInBtn_ = new QPushButton(tr("Sign in"), signInRow_);
    signInBtn_->setObjectName(QStringLiteral("centralMethodsSignIn"));
    signInLayout->addWidget(email_, 1);
    signInLayout->addWidget(password_, 1);
    signInLayout->addWidget(signInBtn_);
    root->addWidget(signInRow_);

    auto* actions = new QHBoxLayout();
    refreshBtn_ = new QPushButton(tr("Refresh"), this);
    refreshBtn_->setObjectName(QStringLiteral("centralMethodsRefresh"));
    cancelBtn_ = new QPushButton(tr("Cancel"), this);
    cancelBtn_->setObjectName(QStringLiteral("centralMethodsCancel"));
    signOutBtn_ = new QPushButton(tr("Sign out"), this);
    signOutBtn_->setObjectName(QStringLiteral("centralMethodsSignOut"));
    auto* closeBtn = new QPushButton(tr("Close"), this);
    actions->addWidget(refreshBtn_);
    actions->addWidget(cancelBtn_);
    actions->addStretch(1);
    actions->addWidget(signOutBtn_);
    actions->addWidget(closeBtn);
    root->addLayout(actions);

    tabs_ = new QTabWidget(this);
    tabs_->setObjectName(QStringLiteral("centralMethodsTabs"));
    auto* methodsPage = new QWidget(tabs_);
    auto* methodsLayout = new QVBoxLayout(methodsPage);
    methodsLayout->setContentsMargins(0, 0, 0, 0);
    table_ = new QTableWidget(0, 7, methodsPage);
    table_->setObjectName(QStringLiteral("centralMethodsTable"));
    table_->setHorizontalHeaderLabels({tr("Method"), tr("Revision"), tr("Project"),
                                       tr("Central state"), tr("Content hash"), tr("Author"),
                                       tr("On this instrument")});
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->verticalHeader()->hide();
    table_->horizontalHeader()->setStretchLastSection(true);
    methodsLayout->addWidget(table_, 1);

    auto* methodActions = new QHBoxLayout();
    applyBtn_ = new QPushButton(tr("Apply..."), this);
    applyBtn_->setObjectName(QStringLiteral("centralMethodsApply"));
    validateBtn_ = new QPushButton(tr("Mark validated..."), this);
    validateBtn_->setObjectName(QStringLiteral("centralMethodsValidate"));
    failedBtn_ = new QPushButton(tr("Record failed run..."), this);
    failedBtn_->setObjectName(QStringLiteral("centralMethodsRecordFailed"));
    methodActions->addWidget(applyBtn_);
    methodActions->addWidget(validateBtn_);
    methodActions->addWidget(failedBtn_);
    methodActions->addStretch(1);
    methodsLayout->addLayout(methodActions);

    auto* reviewActions = new QHBoxLayout();
    const auto reviewButton = [this, reviewActions](const QString& text, const char* name) {
        auto* b = new QPushButton(text, this);
        b->setObjectName(QString::fromLatin1(name));
        reviewActions->addWidget(b);
        return b;
    };
    newDraftBtn_ = reviewButton(tr("New draft..."), "centralMethodsNewDraft");
    approveBtn_ = reviewButton(tr("Approve..."), "centralMethodsApprove");
    rejectBtn_ = reviewButton(tr("Reject..."), "centralMethodsReject");
    publishBtn_ = reviewButton(tr("Publish..."), "centralMethodsPublish");
    archiveBtn_ = reviewButton(tr("Archive..."), "centralMethodsArchive");
    revokeBtn_ = reviewButton(tr("Revoke..."), "centralMethodsRevoke");
    historyBtn_ = reviewButton(tr("History"), "centralMethodsHistory");
    reviewActions->addStretch(1);
    methodsLayout->addLayout(reviewActions);

    details_ = new QPlainTextEdit(methodsPage);
    details_->setObjectName(QStringLiteral("centralMethodsDetails"));
    details_->setReadOnly(true);
    details_->setMaximumHeight(120);
    details_->setPlaceholderText(tr("Select a revision to see its lineage, release notes and history."));
    methodsLayout->addWidget(details_);

    tabs_->addTab(methodsPage, tr("Methods"));
    drafts_ = new MethodDraftsPanel(registry_, hooks_, [this](const QString& text, bool error) { setNotice(text, error); },
                                    tabs_);
    tabs_->addTab(drafts_, tr("Drafts"));
    root->addWidget(tabs_, 1);

    notice_ = new QLabel(this);
    notice_->setObjectName(QStringLiteral("centralMethodsNotice"));
    notice_->setWordWrap(true);
    notice_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    notice_->hide();
    root->addWidget(notice_);

    auto* note = new QLabel(
        tr("Central state is the registry's approval and publication record only. It is not "
           "local validation on this instrument, and listing a method here does not select or "
           "apply it. Apply replaces config.json with the revision exactly (the current file is "
           "backed up); Mark validated needs a test run recorded with that revision applied."),
        this);
    note->setWordWrap(true);
    root->addWidget(note);

    connect(signInBtn_, &QPushButton::clicked, this, &CentralMethodsDialog::signIn);
    connect(password_, &QLineEdit::returnPressed, this, &CentralMethodsDialog::signIn);
    connect(refreshBtn_, &QPushButton::clicked, this, [this] {
        registry_.requestRefresh();
        poll();
    });
    connect(cancelBtn_, &QPushButton::clicked, this, [this] {
        registry_.cancelAll();
        poll();
    });
    connect(signOutBtn_, &QPushButton::clicked, this, [this] {
        registry_.requestSignOut();
        poll();
    });
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    connect(applyBtn_, &QPushButton::clicked, this, &CentralMethodsDialog::apply);
    connect(validateBtn_, &QPushButton::clicked, this, [this] { markValidated(true); });
    connect(failedBtn_, &QPushButton::clicked, this, [this] { markValidated(false); });
    connect(table_, &QTableWidget::itemSelectionChanged, this, [this] {
        const auto s = registry_.snapshot();
        updateActions(s);
        renderDetails(s);
    });
    connect(newDraftBtn_, &QPushButton::clicked, this, &CentralMethodsDialog::newDraftFromRevision);
    connect(approveBtn_, &QPushButton::clicked, this, [this] { transition(CentralState::Approved); });
    connect(rejectBtn_, &QPushButton::clicked, this, [this] { transition(CentralState::Rejected); });
    connect(publishBtn_, &QPushButton::clicked, this, [this] { transition(CentralState::Published); });
    connect(archiveBtn_, &QPushButton::clicked, this, [this] { transition(CentralState::Archived); });
    connect(revokeBtn_, &QPushButton::clicked, this, [this] { transition(CentralState::Revoked); });
    connect(historyBtn_, &QPushButton::clicked, this, [this] {
        const auto id = selectedRevision().toStdString();
        if (!id.empty()) registry_.requestHistory(id);
        poll();
    });

    pollTimer_ = new QTimer(this);
    pollTimer_->setInterval(200);
    connect(pollTimer_, &QTimer::timeout, this, &CentralMethodsDialog::poll);
    poll();
}

void CentralMethodsDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    // Opening the method view is one of the explicit sync points (#398): no
    // background polling of the registry otherwise.
    if (registry_.snapshot().session == RegistryWorkerSnapshot::Session::SignedIn)
        registry_.requestRefresh();
    pollTimer_->start();
    poll();
}

void CentralMethodsDialog::hideEvent(QHideEvent* event) {
    pollTimer_->stop();
    QDialog::hideEvent(event);
}

void CentralMethodsDialog::signIn() {
    const std::string email = email_->text().trimmed().toStdString();
    std::string password = password_->text().toStdString();
    password_->clear(); // the worker owns the only remaining copy
    if (email.empty() || password.empty()) {
        warning_->setText(tr("Enter an email and a password."));
        warning_->show();
        return;
    }
    registry_.requestSignIn(email, std::move(password));
    poll();
}

void CentralMethodsDialog::poll() {
    const auto snapshot = registry_.snapshot();
    if (pendingApplyJob_ != 0) {
        const auto job = registry_.job(pendingApplyJob_);
        if (job.id == 0 || job.terminal()) {
            const auto revision = pendingApplyRevision_;
            pendingApplyJob_ = 0;
            pendingApplyRevision_.clear();
            if (job.state == RegistryJobState::Succeeded) {
                // Render first so the plan reads the materialized dir.
                render(registry_.snapshot());
                continueApply(revision);
                return;
            }
            setNotice(tr("Apply stopped: the revision files could not be materialized (%1).")
                          .arg(q(job.message)),
                      true);
        }
    }
    const std::string config = hooks_.currentConfigJson ? hooks_.currentConfigJson() : std::string{};
    const std::string context =
        hooks_.methodContext ? backend::profiles::methodContextHash(hooks_.methodContext()) : std::string{};
    if (rendered_ && snapshot.generation == renderedGeneration_ && snapshot.busy == renderedBusy_ &&
        config == renderedConfig_ && context == renderedContext_)
        return;
    render(snapshot);
}

QString CentralMethodsDialog::selectedRevision() const {
    const auto rows = table_->selectionModel() ? table_->selectionModel()->selectedRows() : QModelIndexList{};
    if (rows.isEmpty()) return {};
    const auto* item = table_->item(rows.first().row(), 0);
    return item ? item->data(Qt::UserRole).toString() : QString{};
}

void CentralMethodsDialog::setNotice(const QString& text, bool error) {
    notice_->setText(text);
    notice_->setStyleSheet(error ? QStringLiteral("color: #b00020;") : QString{});
    notice_->setVisible(!text.isEmpty());
}

void CentralMethodsDialog::updateActions(const RegistryWorkerSnapshot& s) {
    const auto* r = findRevision(s, selectedRevision().toStdString());
    const bool hasCache = s.session != RegistryWorkerSnapshot::Session::SignedOut;
    const bool signedIn = s.session == RegistryWorkerSnapshot::Session::SignedIn;
    const bool idle = s.configured && !s.busy && pendingApplyJob_ == 0;
    const bool usable = r && usableState(r->state);
    const bool instrumentKnown = hooks_.methodContext && !hooks_.methodContext().instrumentId.empty();

    applyBtn_->setEnabled(idle && hasCache && usable && hooks_.applyConfig && hooks_.currentConfigJson);
    applyBtn_->setToolTip(!hooks_.applyConfig ? tr("No config.json applier in this window")
                          : r && !usable      ? tr("Only published or superseded revisions can be applied")
                                              : tr("Replace config.json with this revision exactly"));
    const bool canValidate = idle && signedIn && usable && hooks_.recordValidation && instrumentKnown;
    validateBtn_->setEnabled(canValidate);
    failedBtn_->setEnabled(canValidate);
    const QString why = !hooks_.recordValidation ? tr("Validation is not available in this window")
                        : !instrumentKnown       ? tr("Instrument identity unknown")
                        : !signedIn              ? tr("Sign in: the validator must be a registry user")
                                                 : tr("Pick a test run recorded with this revision applied");
    validateBtn_->setToolTip(why);
    failedBtn_->setToolTip(why);

    const auto role = [&](const char* name) { return r && backend::profiles::hasProjectRole(s, r->projectId, name); };
    const bool reviewIdle = s.configured && !s.busy && signedIn;
    newDraftBtn_->setEnabled(s.configured && !s.busy && hasCache && r);
    approveBtn_->setEnabled(reviewIdle && role("reviewer") && r->state == CentralState::Submitted);
    rejectBtn_->setEnabled(approveBtn_->isEnabled());
    publishBtn_->setEnabled(reviewIdle && role("publisher") && r->state == CentralState::Approved);
    archiveBtn_->setEnabled(reviewIdle && role("publisher") &&
                            (r->state == CentralState::Published || r->state == CentralState::Superseded));
    revokeBtn_->setEnabled(reviewIdle && role("publisher") &&
                           (r->state == CentralState::Published || r->state == CentralState::Superseded ||
                            r->state == CentralState::Archived));
    historyBtn_->setEnabled(reviewIdle && r);
    const QString reviewWhy = !signedIn ? tr("Sign in to review or publish")
                                        : tr("Needs the reviewer role (approve/reject) or the publisher role");
    for (auto* b : {approveBtn_, rejectBtn_, publishBtn_, archiveBtn_, revokeBtn_})
        b->setToolTip(b->isEnabled() ? tr("Recorded with your reason in the audit trail") : reviewWhy);
}

void CentralMethodsDialog::renderDetails(const RegistryWorkerSnapshot& s) {
    const auto id = selectedRevision().toStdString();
    const auto* r = findRevision(s, id);
    if (!r) {
        details_->clear();
        return;
    }
    QStringList lines;
    const auto* parent = findRevision(s, r->parentRevisionId);
    lines << tr("%1 r%2 · %3 · parent %4")
                 .arg(q(r->displayName))
                 .arg(r->revisionNumber)
                 .arg(stateText(r->state),
                      r->parentRevisionId.empty() ? tr("none")
                      : parent ? QStringLiteral("r%1").arg(parent->revisionNumber)
                               : q(r->parentRevisionId));
    const auto newer = backend::app::newerPublishedRevision(s, *r);
    if (const auto* n = findRevision(s, newer)) lines << tr("Update available: r%1 is published").arg(n->revisionNumber);
    lines << tr("Release notes: %1").arg(r->releaseNotes.empty() ? tr("(none)") : q(r->releaseNotes));
    if (s.history && s.history->revisionId == id) {
        for (const auto& v : s.history->reviews)
            lines << tr("Review: %1 by %2 at %3 - %4").arg(q(v.decision), q(v.reviewerId), q(v.createdAt), q(v.reason));
        for (const auto& e : s.history->events)
            lines << tr("Event: %1 by %2 at %3%4")
                         .arg(q(e.action), q(e.actorId), q(e.createdAt),
                              e.reason.empty() ? QString{} : QStringLiteral(" - ") + q(e.reason));
    }
    details_->setPlainText(lines.join(QLatin1Char('\n')));
}

void CentralMethodsDialog::newDraftFromRevision() {
    const auto id = selectedRevision().toStdString();
    const auto s = registry_.snapshot();
    const auto* r = findRevision(s, id);
    if (!r) return;
    backend::profiles::MethodDraft draft;
    if (hooks_.currentConfigDraft &&
        hooks_.confirm(tr("New draft"), tr("Start the draft from this instrument's current config.json on top of "
                                           "r%1? (No: copy r%1's config.json.)")
                                            .arg(r->revisionNumber))) {
        std::string error;
        draft = hooks_.currentConfigDraft(&error);
        if (!error.empty()) {
            setNotice(tr("Cannot start a draft: %1").arg(q(error)), true);
            return;
        }
    }
    if (registry_.requestSaveDraft(std::move(draft), id) == 0) {
        setNotice(tr("The draft was refused."), true);
        return;
    }
    setNotice(tr("Draft created from r%1; edit and submit it on the Drafts tab.").arg(r->revisionNumber), false);
    tabs_->setCurrentWidget(drafts_);
}

void CentralMethodsDialog::transition(CentralState target) {
    const auto id = selectedRevision().toStdString();
    const auto s = registry_.snapshot();
    const auto* r = findRevision(s, id);
    if (!r) return;
    const QString verb = target == CentralState::Approved   ? tr("Approve")
                         : target == CentralState::Rejected  ? tr("Reject")
                         : target == CentralState::Published ? tr("Publish")
                         : target == CentralState::Archived  ? tr("Archive")
                                                             : tr("Revoke");
    QString label = tr("%1 \"%2\" r%3. Reason (recorded in the audit trail):")
                        .arg(verb, q(r->displayName))
                        .arg(r->revisionNumber);
    if (target == CentralState::Revoked)
        label += QStringLiteral("\n") + tr("Revocation is permanent: every instrument blocks Start with it.");
    const auto reason = hooks_.askText(verb, label, {}, true);
    if (!reason || reason->trimmed().isEmpty()) {
        if (reason) setNotice(tr("A reason is required; nothing was changed."), true);
        return;
    }
    if (registry_.requestTransition(id, target, reason->toStdString()) == 0) {
        setNotice(tr("The request was refused."), true);
        return;
    }
    setNotice(tr("%1 requested...").arg(verb), false);
    poll();
}

void CentralMethodsDialog::apply() {
    const auto id = selectedRevision().toStdString();
    const auto s = registry_.snapshot();
    const auto* r = findRevision(s, id);
    if (!r) return;
    setNotice({}, false);
    if (!r->materializedDir.empty()) {
        continueApply(id);
        return;
    }
    pendingApplyJob_ = registry_.requestMaterialize(id);
    pendingApplyRevision_ = id;
    if (pendingApplyJob_ == 0) {
        pendingApplyRevision_.clear();
        setNotice(tr("This revision cannot be materialized."), true);
        return;
    }
    setNotice(tr("Preparing revision files..."), false);
    updateActions(registry_.snapshot());
}

void CentralMethodsDialog::continueApply(const std::string& revisionId) {
    const auto plan = backend::app::planMethodApply(registry_.snapshot(), revisionId,
                                                    hooks_.currentConfigJson());
    if (!plan.ok) {
        setNotice(tr("Cannot apply: %1").arg(q(plan.error)), true);
        return;
    }
    QStringList changes;
    for (int i = 0; i < static_cast<int>(plan.changedKeys.size()) && i < kChangesShown; ++i)
        changes << QStringLiteral("  • ") + q(plan.changedKeys[static_cast<size_t>(i)]);
    if (static_cast<int>(plan.changedKeys.size()) > kChangesShown)
        changes << tr("  ... and %1 more").arg(static_cast<int>(plan.changedKeys.size()) - kChangesShown);
    const QString summary =
        plan.changedKeys.empty()
            ? tr("config.json already has these values; it will be rewritten byte-for-byte.")
            : tr("These config.json settings change (instrument settings such as COM ports or the "
                 "save directory travel with the method):\n%1")
                  .arg(changes.join(QStringLiteral("\n")));
    const QString text =
        tr("Apply \"%1\" r%2 (%3)?\n\nconfig.json is replaced with this revision exactly; the "
           "current file is backed up first.\n\n%4\n\nThe camera script is not applied "
           "automatically: %5")
            .arg(q(plan.displayName))
            .arg(plan.revisionNumber)
            .arg(q(plan.centralState), summary, q(plan.cameraScriptPath));
    if (!hooks_.confirm(tr("Apply central method"), text)) {
        setNotice(tr("Apply cancelled; nothing was changed."), false);
        return;
    }
    QString backup;
    const QString error = hooks_.applyConfig(QByteArray::fromStdString(plan.configText), &backup);
    if (!error.isEmpty()) {
        setNotice(tr("Apply failed: %1").arg(error), true);
        return;
    }
    setNotice(backup.isEmpty() ? tr("Applied \"%1\" r%2.").arg(q(plan.displayName)).arg(plan.revisionNumber)
                               : tr("Applied \"%1\" r%2. Previous config.json saved as %3.")
                                     .arg(q(plan.displayName))
                                     .arg(plan.revisionNumber)
                                     .arg(backup),
              false);
    render(registry_.snapshot());
}

void CentralMethodsDialog::markValidated(bool passed) {
    const auto id = selectedRevision().toStdString();
    if (id.empty() || !hooks_.recordValidation) return;
    const QString file = hooks_.pickEvidenceFile(
        passed ? tr("Test run that validates this revision") : tr("Test run that failed with this revision"));
    if (file.isEmpty()) return;
    const auto outcome = hooks_.recordValidation(id, file.toStdString(), passed);
    if (outcome.jobId == 0) {
        setNotice(tr("Not recorded: %1").arg(q(outcome.error)), true);
        return;
    }
    setNotice(passed ? tr("Recording the local validation...") : tr("Recording the failed run..."), false);
    poll();
}

void CentralMethodsDialog::render(const RegistryWorkerSnapshot& s) {
    rendered_ = true;
    renderedGeneration_ = s.generation;
    renderedBusy_ = s.busy;
    renderedConfig_ = hooks_.currentConfigJson ? hooks_.currentConfigJson() : std::string{};
    const auto context = hooks_.methodContext ? hooks_.methodContext() : backend::profiles::MethodContext{};
    const auto contextHash = backend::profiles::methodContextHash(context);
    renderedContext_ = contextHash;
    const auto appliedConfigSha =
        renderedConfig_.empty() ? std::string{} : backend::profiles::canonicalConfigSha256(renderedConfig_);
    if (!hooks_.methodContext)
        instrument_->setText({});
    else if (context.instrumentId.empty())
        instrument_->setText(tr("Instrument identity unknown: local validation is unavailable"));
    else
        instrument_->setText(hooks_.instrumentName.empty()
                                 ? tr("Instrument: %1").arg(q(context.instrumentId))
                                 : tr("Instrument: %1 (%2)").arg(q(hooks_.instrumentName), q(context.instrumentId)));

    status_->setText(connectivityText(s));
    account_->setText(accountText(s));

    QStringList activity;
    if (s.busy) activity << tr("Working...");
    if (s.lastSuccessfulRefresh) {
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                              s.lastSuccessfulRefresh->time_since_epoch())
                              .count();
        activity << tr("Last refresh: %1")
                        .arg(QDateTime::fromSecsSinceEpoch(secs).toString(Qt::ISODate));
    }
    const QString job = jobText(s.lastJob);
    if (!job.isEmpty()) activity << job;
    activity_->setText(activity.join(QStringLiteral(" | ")));

    QStringList warnings;
    if (!s.cacheError.empty()) warnings << q(s.cacheError);
    if (!s.corruptRevisionIds.empty())
        warnings << tr("%n cached revision(s) failed integrity checks and are hidden.", nullptr,
                       static_cast<int>(s.corruptRevisionIds.size()));
    if (s.health.rejectedRevisions > 0)
        warnings << tr("%n registry revision(s) failed verification and were not cached.", nullptr,
                       static_cast<int>(s.health.rejectedRevisions));
    if (s.configured && !s.health.message.empty() &&
        s.health.connectivity != RegistryHealth::Connectivity::Online)
        warnings << q(s.health.message);
    warning_->setText(warnings.join(QStringLiteral("\n")));
    warning_->setVisible(!warnings.isEmpty());

    const bool signedIn = s.session == RegistryWorkerSnapshot::Session::SignedIn;
    const bool hasUser = s.session != RegistryWorkerSnapshot::Session::SignedOut;
    signInRow_->setVisible(s.configured && !signedIn);
    signInRow_->setEnabled(s.configured);
    signInBtn_->setEnabled(s.configured && !s.busy);
    refreshBtn_->setEnabled(s.configured && signedIn && !s.busy);
    cancelBtn_->setEnabled(s.configured && (s.busy || s.queuedJobs > 0));
    signOutBtn_->setEnabled(s.configured && hasUser && !s.busy);
    if (hasUser && email_->text().isEmpty() && !s.email.empty()) email_->setText(q(s.email));

    QMap<QString, QString> projectNames;
    for (const auto& p : s.projects)
        projectNames.insert(q(p.projectId), q(p.displayName));

    const QString keepSelection = selectedRevision();
    const QSignalBlocker blockSelection(table_);
    table_->setRowCount(static_cast<int>(s.revisions.size()));
    for (int row = 0; row < static_cast<int>(s.revisions.size()); ++row) {
        const auto& r = s.revisions[static_cast<size_t>(row)];
        const QString project = q(r.projectId);
        const QStringList cells{
            q(r.displayName),
            QStringLiteral("r%1").arg(r.revisionNumber),
            projectNames.value(project, project),
            stateText(r.state),
            q(r.contentHash.substr(0, 12)),
            q(r.authorId),
            [&] {
                QStringList parts;
                if (!appliedConfigSha.empty() && r.configSha256 == appliedConfigSha)
                    parts << tr("APPLIED");
                if (hooks_.methodContext) {
                    const auto local = backend::app::localValidationFor(s, r, context.instrumentId, contextHash);
                    switch (local.state) {
                    case backend::app::LocalValidationState::Passed:
                        parts << tr("validated by %1 (%2 UTC)").arg(q(local.validatorId), q(local.validatedAtUtc));
                        break;
                    case backend::app::LocalValidationState::Failed:
                        parts << tr("validation FAILED (%1, %2 UTC)").arg(q(local.validatorId), q(local.validatedAtUtc));
                        break;
                    case backend::app::LocalValidationState::None:
                        parts << tr("not validated");
                        break;
                    }
                }
                if (!r.materializedDir.empty()) parts << tr("files ready");
                if (const auto* newer = findRevision(s, backend::app::newerPublishedRevision(s, r)))
                    parts << tr("r%1 available").arg(newer->revisionNumber);
                return parts.join(QStringLiteral(" · "));
            }(),
        };
        for (int col = 0; col < cells.size(); ++col) {
            auto* item = new QTableWidgetItem(cells[col]);
            if (col == 4) item->setToolTip(q(r.contentHash));
            if (col == 0 && !r.releaseNotes.empty()) item->setToolTip(q(r.releaseNotes));
            item->setData(Qt::UserRole, q(r.revisionId));
            if (r.state == CentralState::Revoked) {
                QFont f = item->font();
                f.setBold(true);
                item->setFont(f);
                item->setForeground(QColor(QStringLiteral("#b00020")));
            }
            table_->setItem(row, col, item);
        }
        if (q(r.revisionId) == keepSelection) table_->selectRow(row);
    }
    table_->resizeColumnsToContents();
    updateActions(s);
    renderDetails(s);
    drafts_->render(s);
}

} // namespace frontend
