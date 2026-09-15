#include "frontend/system/DesktopInstance.h"
#include "support/assert.h"
#include "support/watchdog.h"
#include <QCoreApplication>
#include <QProcess>
#include <QTemporaryDir>
#include <cstdio>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() == 3) {
        frontend::DesktopInstance owner(args[2]);
        if (!owner.acquire()) return 7;
        if (args[1] == "--hold") {
            std::puts("locked");
            std::fflush(stdout);
            return app.exec();
        }
        return 0;
    }
    mib::test::Watchdog watchdog(20);
    QTemporaryDir dir;
    MIB_REQUIRE(dir.isValid(), "temp directory");
    const auto path = dir.filePath("desktop.lock");
    auto runContender = [&] {
        QProcess contender;
        contender.start(app.applicationFilePath(), {"--try", path});
        if (!contender.waitForFinished(5000)) {
            contender.kill();
            contender.waitForFinished(1000);
            return -1;
        }
        return contender.exitCode();
    };
    {
        frontend::DesktopInstance owner(path);
        MIB_REQUIRE(owner.acquire(), "first launch reserves desktop");
        for (int i = 0; i < 10; ++i)
            MIB_EXPECT(runContender() == 7, "duplicate launch rejected");
    }
    MIB_EXPECT(runContender() == 0, "clean exit releases lock");
    QProcess crashed;
    crashed.start(app.applicationFilePath(), {"--hold", path});
    MIB_REQUIRE(crashed.waitForReadyRead(5000), "child owns lock");
    MIB_REQUIRE(crashed.readAllStandardOutput().contains("locked"), "owner ready");
    MIB_EXPECT(runContender() == 7, "live child protects hardware");
    crashed.kill();
    MIB_REQUIRE(crashed.waitForFinished(5000), "simulated crash exits");
    MIB_EXPECT(runContender() == 0, "dead owner lock recovered automatically");
    return mib::test::exitCode();
}
