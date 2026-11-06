// experiment_save_location_test (issue #906)
//
// The Start Experiment save dialog's default folder: a relative profile
// save_directory resolves under <Documents>/MIB_Studio_Qt (never the working
// directory / install folder), an absolute one is kept, and "" or ".." gives
// the data root.

#include "frontend/utils/ExperimentSaveLocation.h"

#include "support/assert.h"

#include <QDir>

int main()
{
    using frontend::resolveExperimentSaveDirectory;
#ifdef _WIN32
    const QString docs = QStringLiteral("C:/Users/u/Documents"); // "/home/..." is drive-relative on Windows
#else
    const QString docs = QStringLiteral("/home/u/Documents");
#endif
    const QString root = docs + QStringLiteral("/MIB_Studio_Qt");

    MIB_EXPECT(resolveExperimentSaveDirectory(QStringLiteral("testing"), docs) == root + QStringLiteral("/testing"),
               "relative save_directory resolves under Documents/MIB_Studio_Qt");
    MIB_EXPECT(resolveExperimentSaveDirectory(QStringLiteral("runs\\day1"), docs) == root + QStringLiteral("/runs/day1"),
               "Windows separators in a relative value");
    MIB_EXPECT(resolveExperimentSaveDirectory(QString(), docs) == root, "empty -> data root");
    MIB_EXPECT(resolveExperimentSaveDirectory(QStringLiteral("  "), docs) == root, "blank -> data root");
    MIB_EXPECT(resolveExperimentSaveDirectory(QStringLiteral("../../x"), docs) == root, "climbing out of the root -> data root");
    MIB_EXPECT(resolveExperimentSaveDirectory(QStringLiteral("/data/mib/"), docs) == QStringLiteral("/data/mib"),
               "absolute save_directory kept");
#ifdef _WIN32
    MIB_EXPECT(resolveExperimentSaveDirectory(QStringLiteral("D:\\mib data"), QStringLiteral("C:/Users/u/Documents"))
                   == QStringLiteral("D:/mib data"),
               "absolute drive path kept");
#endif
    MIB_EXPECT(resolveExperimentSaveDirectory(QStringLiteral("testing"), QString()).startsWith(QDir::homePath()),
               "no Documents location -> under the home directory");
    return mib::test::exitCode();
}
