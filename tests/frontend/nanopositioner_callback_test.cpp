#include "backend/app/AppBackend.h"
#include "backend/services/AutofocusService.h"
#include "frontend/tabs/NanopositionerTab.h"
#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"
#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include <QLabel>
#include <QSettings>
#include <fstream>
#include <nlohmann/json.hpp>
#include <thread>

namespace backend::services {
struct AutofocusCallbackTestAccess {
    static AutofocusService::StatusCallback snapshot(AutofocusService& service) {
        std::scoped_lock lock(service.callbackMutex_);
        return service.statusCallback_;
    }
};
} // namespace backend::services
namespace {
class FakeNano final : public backend::nanopositioner::INanopositionerBackend {
public:
    explicit FakeNano(double voltage = 0) : voltage_(voltage) {}
    backend::nanopositioner::BackendKind kind() const override {
        return backend::nanopositioner::BackendKind::Oeabt;
    }
    bool connect(const backend::nanopositioner::Endpoint&, std::string&) override { return true; }
    void disconnect() override {}
    bool isConnected() const override { return true; }
    bool readVoltage(double& v, std::string&) override {
        v = voltage_;
        return true;
    }
    bool setVoltage(double, std::string&) override { return true; }
    std::optional<double> maximumVoltage() const override { return 100; }
    std::string connectedEndpoint() const override { return "fake"; }

private:
    double voltage_;
};
} // namespace
int main(int argc, char** argv) {
    qputenv("MIB_DISABLED_SERVICES",
            "auto_update,trigger,syringe_pump,pulse_generator,capture,processing");
    QApplication app(argc, argv);
    mib::test::Watchdog watchdog(30);
    mib::test::TempDir temp("nano_callback");
    QCoreApplication::setOrganizationName("MIB-tests");
    QCoreApplication::setApplicationName("callback-lifetime");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       QString::fromStdString(temp.path().string()));
    const auto configPath = temp / "config.json";
    std::ofstream(configPath) << R"({"autofocus_initial_voltage":47.5})";
    QSettings().setValue("Config/ExternalAppConfigPath",
                         QString::fromStdString(configPath.string()));
    backend::AppBackend backend;
    MIB_REQUIRE(backend.initialize((temp / "data").string()),
                "backend initializes without hardware");
    MIB_REQUIRE(
        backend.autofocus().setBackendFactory([](auto) { return std::make_unique<FakeNano>(); }),
        "fake driver");
    auto* tab = new frontend::NanopositionerTab(backend);
    backend::nanopositioner::Endpoint endpoint;
    endpoint.backend = backend::nanopositioner::BackendKind::Oeabt;
    endpoint.persistentId = "fake";
    MIB_REQUIRE(backend.autofocus().connect(endpoint), "observe-only fake connect");
    tab->applyAutoConnectResult(endpoint);
    nlohmann::json config;
    std::ifstream(configPath) >> config;
    MIB_EXPECT(config.at("autofocus_initial_voltage") == 47.5,
               "connect preserves configured voltage over observed zero");
    QMetaObject::invokeMethod(tab, "onTargetRingWidthChanged", Qt::DirectConnection,
                              Q_ARG(double, 21.0));
    std::ifstream(configPath) >> config;
    MIB_EXPECT(config.at("autofocus_initial_voltage") == 47.5,
               "unrelated edits preserve configured voltage");
    backend.autofocus().disconnect();
    MIB_REQUIRE(backend.autofocus().setBackendFactory(
                    [](auto) { return std::make_unique<FakeNano>(12.0); }),
                "nonzero fake driver");
    MIB_REQUIRE(backend.autofocus().connect(endpoint), "observe nonzero voltage");
    tab->applyAutoConnectResult(endpoint);
    std::ifstream(configPath) >> config;
    MIB_EXPECT(config.at("autofocus_initial_voltage") == 47.5,
               "nonzero observation also preserves configured voltage");
    // #907: auto-connect at launch and aboutToQuit at close do not rewrite
    // an unchanged config. A CRLF profile (Windows) keeps its bytes and mtime.
    {
        const QString cfg = QString::fromStdString(configPath.string());
        auto bytesOf = [&cfg] {
            QFile f(cfg);
            return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
        };
        QByteArray crlf = bytesOf();
        crlf.replace("\n", "\r\n");
        {
            QFile f(cfg);
            MIB_REQUIRE(f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(crlf) == crlf.size(),
                        "CRLF profile fixture");
        }
        const QDateTime old = QDateTime::fromSecsSinceEpoch(1577934245); // 2020-01-02
        auto stampOld = [&] {
            QFile f(cfg);
            MIB_REQUIRE(f.open(QIODevice::ReadWrite) && f.setFileTime(old, QFileDevice::FileModificationTime),
                        "set old mtime");
        };
        auto untouched = [&] {
            return bytesOf() == crlf && QFileInfo(cfg).lastModified().toSecsSinceEpoch() == old.toSecsSinceEpoch();
        };
        stampOld();
        tab->applyAutoConnectResult(endpoint);
        MIB_EXPECT(untouched(), "auto-connect with nothing changed leaves the CRLF profile untouched");
        stampOld();
        QTimer::singleShot(0, &app, &QCoreApplication::quit);
        app.exec(); // aboutToQuit -> saveConfig
        MIB_EXPECT(untouched(), "quit with nothing changed leaves the CRLF profile untouched");
        auto* setpoint = tab->findChild<QDoubleSpinBox*>("targetRingWidthSpinBox");
        MIB_REQUIRE(setpoint, "setpoint spin box");
        const double target = setpoint->value() + 0.5 <= setpoint->maximum() ? setpoint->value() + 0.5 : setpoint->value() - 0.5;
        setpoint->setValue(target); // valueChanged -> onTargetRingWidthChanged -> saveConfig
        std::ifstream(configPath) >> config;
        MIB_EXPECT(config.at("autofocus_focus_setpoint") == setpoint->value() && config.at("autofocus_initial_voltage") == 47.5
                       && !bytesOf().contains('\r'),
                   "a real change is still written (binary, LF)");
    }
    backend.autofocus().disconnect();
    for (int cycle = 0; cycle < 50; ++cycle) {
        if (cycle) tab = new frontend::NanopositionerTab(backend);
        auto callback =
            backend::services::AutofocusCallbackTestAccess::snapshot(backend.autofocus());
        auto* label = tab->findChild<QLabel*>("statusLabel");
        MIB_REQUIRE(label, "status label");
        label->setText("sentinel");
        std::thread once([&] { callback("worker status"); });
        once.join();
        MIB_EXPECT(label->text() == "sentinel", "worker does not change widgets");
        QCoreApplication::processEvents();
        MIB_EXPECT(label->text() == "worker status", "queued delivery reaches live tab");
        std::atomic<bool> started{false};
        std::thread producer([&] {
            for (int i = 0; i < 200; ++i) {
                callback("concurrent status");
                started.store(true);
            }
        });
        while (!started.load())
            std::this_thread::yield();
        delete tab;
        producer.join();
        // A snapshot already taken by notifyStatus survives unregistration.
        callback("late snapshot after destruction");
        QCoreApplication::processEvents();
    }
    return mib::test::exitCode();
}
