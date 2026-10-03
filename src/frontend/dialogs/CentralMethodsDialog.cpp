#include "frontend/dialogs/CentralMethodsDialog.h"

#include "backend/profiles/ProfileRegistryWorker.h"

#include <QDateTime>
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

} // namespace

CentralMethodsDialog::CentralMethodsDialog(backend::profiles::ProfileRegistryWorker& registry,
                                           QWidget* parent)
    : QDialog(parent), registry_(registry) {
    setWindowTitle(tr("Central Methods"));
    resize(820, 480);
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

    table_ = new QTableWidget(0, 6, this);
    table_->setObjectName(QStringLiteral("centralMethodsTable"));
    table_->setHorizontalHeaderLabels({tr("Method"), tr("Revision"), tr("Project"),
                                       tr("Central state"), tr("Content hash"), tr("Author")});
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->verticalHeader()->hide();
    table_->horizontalHeader()->setStretchLastSection(true);
    root->addWidget(table_, 1);

    auto* note = new QLabel(
        tr("Central state is the registry's approval and publication record only. It is not "
           "local validation on this instrument, and listing a method here does not select or "
           "apply it."),
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
    if (rendered_ && snapshot.generation == renderedGeneration_ && snapshot.busy == renderedBusy_)
        return;
    render(snapshot);
}

void CentralMethodsDialog::render(const RegistryWorkerSnapshot& s) {
    rendered_ = true;
    renderedGeneration_ = s.generation;
    renderedBusy_ = s.busy;

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
        };
        for (int col = 0; col < cells.size(); ++col) {
            auto* item = new QTableWidgetItem(cells[col]);
            if (col == 4) item->setToolTip(q(r.contentHash));
            item->setData(Qt::UserRole, q(r.revisionId));
            if (r.state == CentralState::Revoked) {
                QFont f = item->font();
                f.setBold(true);
                item->setFont(f);
                item->setForeground(QColor(QStringLiteral("#b00020")));
            }
            table_->setItem(row, col, item);
        }
    }
    table_->resizeColumnsToContents();
}

} // namespace frontend
