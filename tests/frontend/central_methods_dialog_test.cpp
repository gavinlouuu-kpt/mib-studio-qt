// CentralMethodsDialog (#398 M1), offscreen, real ProfileRegistryWorker over a
// fake Supabase: unconfigured state, rejected and accepted sign-in through the
// widgets (password field cleared), refresh-on-open, revoked rows marked, an
// outage keeps the cached rows, the GUI stays responsive while a registry
// request hangs and Cancel aborts it, sign-out, and the last user's cached
// methods listed offline after a restart.
#include "frontend/dialogs/CentralMethodsDialog.h"

#include "backend/profiles/ProfileRegistryWorker.h"

#include "support/assert.h"
#include "support/fake_supabase.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>

#include <functional>

using namespace backend::profiles;
using mib::test::FakeSupabase;
using mib::test::revisionJson;
using mib::test::transportFor;

namespace {

bool waitUntil(const std::function<bool()>& pred, int timeoutMs = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (!pred()) {
        if (timer.elapsed() > timeoutMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

RegistryWorkerConfig config(const std::filesystem::path& dir) {
    RegistryWorkerConfig c;
    c.origin = mib::test::kOrigin;
    c.publishableKey = mib::test::kKey;
    c.cacheDir = dir;
    return c;
}

template <class T> T* find(QWidget& root, const char* name) {
    auto* w = root.findChild<T*>(QString::fromLatin1(name));
    MIB_REQUIRE(w != nullptr, std::string("missing widget ") + name);
    return w;
}

QString cell(QTableWidget* table, int row, int col) {
    auto* item = table->item(row, col);
    return item ? item->text() : QString();
}

int rowWithState(QTableWidget* table, const QString& prefix) {
    for (int r = 0; r < table->rowCount(); ++r)
        if (cell(table, r, 3).startsWith(prefix)) return r;
    return -1;
}

} // namespace

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    mib::test::Watchdog watchdog(60);
    mib::test::TempDir dir("mib_central_methods_ui");
    FakeSupabase fake;
    fake.users["alice@lab"] = {"user-alice", "pw-alice", {"p1"}};
    fake.revisions["p1"] = {revisionJson("p1", "r1", 1, "superseded"),
                            revisionJson("p1", "r2", 2, "published"),
                            revisionJson("p1", "r3", 3, "revoked")};

    watchdog.mark("unconfigured");
    {
        ProfileRegistryWorker inert({}, transportFor(fake));
        frontend::CentralMethodsDialog dialog(inert);
        dialog.show();
        waitUntil([] { return false; }, 100);
        MIB_EXPECT(find<QLabel>(dialog, "centralMethodsStatus")->text().contains("not configured"),
                   "unconfigured registry is said plainly");
        MIB_EXPECT(!find<QPushButton>(dialog, "centralMethodsRefresh")->isEnabled() &&
                       !find<QLineEdit>(dialog, "centralMethodsEmail")->isVisible(),
                   "no registry controls without configuration");
    }

    watchdog.mark("sign in through the dialog");
    {
        ProfileRegistryWorker worker(config(dir.path()), transportFor(fake));
        MIB_REQUIRE(worker.waitIdle(std::chrono::seconds(5)), "worker idle");
        frontend::CentralMethodsDialog dialog(worker);
        dialog.show();
        auto* email = find<QLineEdit>(dialog, "centralMethodsEmail");
        auto* password = find<QLineEdit>(dialog, "centralMethodsPassword");
        auto* signIn = find<QPushButton>(dialog, "centralMethodsSignIn");
        auto* refresh = find<QPushButton>(dialog, "centralMethodsRefresh");
        auto* cancel = find<QPushButton>(dialog, "centralMethodsCancel");
        auto* signOut = find<QPushButton>(dialog, "centralMethodsSignOut");
        auto* table = find<QTableWidget>(dialog, "centralMethodsTable");
        auto* status = find<QLabel>(dialog, "centralMethodsStatus");
        auto* account = find<QLabel>(dialog, "centralMethodsAccount");
        auto* warning = find<QLabel>(dialog, "centralMethodsWarning");

        MIB_EXPECT(email->isVisible() && !refresh->isEnabled(), "signed out: sign-in shown");

        email->setText(QStringLiteral("alice@lab"));
        password->setText(QStringLiteral("wrong"));
        signIn->click();
        MIB_EXPECT(password->text().isEmpty(), "password field cleared on submit");
        MIB_REQUIRE(waitUntil([&] { return worker.snapshot().lastJob.id != 0; }), "sign-in ran");
        MIB_REQUIRE(waitUntil([&] { return warning->isVisible(); }), "rejection shown");
        MIB_EXPECT(status->text().contains("sign-in required"), "bad password: sign-in required");
        MIB_EXPECT(account->text() == QStringLiteral("Not signed in"), "still signed out");

        password->setText(QStringLiteral("pw-alice"));
        signIn->click();
        MIB_REQUIRE(waitUntil([&] { return account->text().contains("Signed in as alice@lab"); }),
                    "signed in through the dialog");
        MIB_EXPECT(!email->isVisible() && signOut->isEnabled(), "sign-in row hidden once in");

        watchdog.mark("refresh");
        MIB_REQUIRE(waitUntil([&] { return refresh->isEnabled(); }), "refresh available");
        refresh->click();
        MIB_REQUIRE(waitUntil([&] { return table->rowCount() == 3 && !worker.snapshot().busy; }),
                    "three cached revisions listed");
        const int revoked = rowWithState(table, QStringLiteral("REVOKED"));
        MIB_EXPECT(revoked >= 0 && table->item(revoked, 0)->font().bold(),
                   "revoked revision marked prominently");
        MIB_EXPECT(rowWithState(table, QStringLiteral("Superseded")) >= 0 &&
                       rowWithState(table, QStringLiteral("Published")) >= 0,
                   "each central state shown as itself");
        MIB_EXPECT(cell(table, 0, 2) == QStringLiteral("Project p1"),
                   "project display name, not its ID");
        MIB_EXPECT(status->text().contains("online"), "online after refresh");

        watchdog.mark("outage");
        {
            std::lock_guard<std::mutex> lock(fake.mutex);
            fake.offline = true;
        }
        refresh->click();
        MIB_REQUIRE(waitUntil([&] { return status->text().contains("offline"); }),
                    "outage shown as registry offline");
        MIB_EXPECT(table->rowCount() == 3, "cached rows stay listed during an outage");
        {
            std::lock_guard<std::mutex> lock(fake.mutex);
            fake.offline = false;
        }

        watchdog.mark("hung request keeps the GUI responsive");
        MIB_REQUIRE(waitUntil([&] { return refresh->isEnabled(); }), "idle again");
        fake.hang = true;
        const int hungBefore = fake.hungRequests.load();
        refresh->click();
        MIB_REQUIRE(waitUntil([&] { return fake.hungRequests.load() > hungBefore; }),
                    "request in flight");
        int ticks = 0;
        QTimer ticker;
        QObject::connect(&ticker, &QTimer::timeout, [&] { ++ticks; });
        ticker.start(10);
        waitUntil([] { return false; }, 300);
        MIB_EXPECT(ticks >= 10, "GUI thread keeps running while the registry hangs");
        MIB_REQUIRE(waitUntil([&] { return cancel->isEnabled(); }), "cancel offered while busy");
        cancel->click();
        MIB_REQUIRE(waitUntil([&] { return !worker.snapshot().busy; }), "cancel unblocks");
        fake.hang = false;
        MIB_EXPECT(table->rowCount() == 3, "cancel leaves the cache listed");
    }

    watchdog.mark("restart offline");
    {
        {
            std::lock_guard<std::mutex> lock(fake.mutex);
            fake.offline = true;
        }
        ProfileRegistryWorker worker(config(dir.path()), transportFor(fake));
        MIB_REQUIRE(worker.waitIdle(std::chrono::seconds(5)), "worker idle");
        frontend::CentralMethodsDialog dialog(worker);
        dialog.show();
        auto* table = find<QTableWidget>(dialog, "centralMethodsTable");
        MIB_REQUIRE(waitUntil([&] { return table->rowCount() == 3; }),
                    "last user's methods listed offline after restart");
        MIB_EXPECT(find<QLabel>(dialog, "centralMethodsAccount")->text().contains("Cached methods"),
                   "offline cache labelled as such");
        MIB_EXPECT(!find<QPushButton>(dialog, "centralMethodsRefresh")->isEnabled(),
                   "refresh needs a sign-in");
        {
            std::lock_guard<std::mutex> lock(fake.mutex);
            fake.offline = false;
        }

        watchdog.mark("refresh on open");
        auto* password = find<QLineEdit>(dialog, "centralMethodsPassword");
        MIB_EXPECT(find<QLineEdit>(dialog, "centralMethodsEmail")->text() ==
                       QStringLiteral("alice@lab"),
                   "email prefilled from the cached session");
        password->setText(QStringLiteral("pw-alice"));
        find<QPushButton>(dialog, "centralMethodsSignIn")->click();
        MIB_REQUIRE(waitUntil([&] {
                        return worker.snapshot().session ==
                               RegistryWorkerSnapshot::Session::SignedIn;
                    }),
                    "signed back in");
        dialog.hide();
        MIB_REQUIRE(worker.waitIdle(std::chrono::seconds(5)), "idle");
        const int before = fake.requests.load();
        dialog.show();
        MIB_REQUIRE(waitUntil([&] { return fake.requests.load() > before; }),
                    "opening the dialog refreshes when signed in");

        watchdog.mark("sign out");
        MIB_REQUIRE(worker.waitIdle(std::chrono::seconds(5)), "idle");
        auto* signOut = find<QPushButton>(dialog, "centralMethodsSignOut");
        MIB_REQUIRE(waitUntil([&] { return signOut->isEnabled(); }), "sign-out offered");
        signOut->click();
        MIB_REQUIRE(waitUntil([&] { return table->rowCount() == 0; }),
                    "sign-out clears the listed methods");
        MIB_EXPECT(find<QLineEdit>(dialog, "centralMethodsEmail")->isVisible(),
                   "sign-in offered again");
    }
    return mib::test::exitCode();
}
