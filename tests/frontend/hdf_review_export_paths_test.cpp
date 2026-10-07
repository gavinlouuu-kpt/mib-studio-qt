#include "frontend/utils/HdfReviewExportPaths.h"

#include "support/assert.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cstdio>

namespace paths = frontend::hdfreviewexport;

namespace {

void touch(const QString& path)
{
    QFile file(path);
    MIB_REQUIRE(file.open(QIODevice::WriteOnly), "fixture file opens for writing");
    file.write("x");
}

} // namespace

int main()
{
    QTemporaryDir temp;
    MIB_REQUIRE(temp.isValid(), "temporary export root is valid");
    QDir root(temp.path());

    MIB_EXPECT(paths::sourceBaseName(root.filePath("experiment.h5")) == "experiment",
               "h5 suffix stripped from source basename");
    MIB_EXPECT(paths::sourceBaseName(root.filePath("cell run.v1.hdf5")) == "cell run.v1",
               "dotted and spaced hdf5 basename preserved");

    const QString firstMetrics = paths::metricsCsvPath(root.filePath("sample.h5"), root.path());
    MIB_EXPECT(QFileInfo(firstMetrics).fileName() == "sample_metrics.csv",
               "single metrics suggestion uses source basename");

    touch(root.filePath("sample_metrics.csv"));
    const QString secondMetrics = paths::metricsCsvPath(root.filePath("sample.h5"), root.path());
    MIB_EXPECT(QFileInfo(secondMetrics).fileName() == "sample_metrics_2.csv",
               "existing metrics suggestion is suffixed");
    touch(root.filePath("sample_metrics_2.csv"));
    const QString thirdMetrics = paths::metricsCsvPath(root.filePath("sample.h5"), root.path());
    MIB_EXPECT(QFileInfo(thirdMetrics).fileName() == "sample_metrics_3.csv",
               "metrics suffix advances past multiple collisions");

    const QString firstFcs = paths::fcsPath(root.filePath("sample.h5"), root.path());
    MIB_EXPECT(QFileInfo(firstFcs).fileName() == "sample.fcs", "single FCS suggestion uses source basename");
    touch(root.filePath("sample_event_map.csv"));
    const QString secondFcs = paths::fcsPath(root.filePath("sample.h5"), root.path());
    MIB_EXPECT(QFileInfo(secondFcs).fileName() == "sample_2.fcs", "FCS suggestion reserves its event map");
    touch(root.filePath("sample_2.fcs"));
    touch(root.filePath("sample_2_event_map.csv"));

    const QString allDir = paths::exportAllDirectoryPath(root.filePath("sample.h5"), root.path());
    MIB_EXPECT(QFileInfo(allDir).fileName() == "sample",
               "export-all suggestion uses source-specific folder");
    MIB_REQUIRE(root.mkdir("sample"), "fixture export-all folder created");
    const QString allDir2 = paths::exportAllDirectoryPath(root.filePath("sample.h5"), root.path());
    MIB_EXPECT(QFileInfo(allDir2).fileName() == "sample_2",
               "export-all folder collision is suffixed");

    const QStringList duplicateBatch{
        root.filePath("sample.h5"),
        root.filePath("other/sample.h5"),
        root.filePath("cell run.v1.hdf5")
    };
    const QStringList metricBatch = paths::batchMetricsCsvPaths(duplicateBatch, root.path());
    MIB_REQUIRE(metricBatch.size() == 3, "batch metrics produced three paths");
    MIB_EXPECT(QFileInfo(metricBatch[0]).fileName() == "sample_metrics_3.csv",
               "batch metrics respects existing collisions");
    MIB_EXPECT(QFileInfo(metricBatch[1]).fileName() == "sample_metrics_4.csv",
               "batch metrics reserves earlier suggestions within the run");
    MIB_EXPECT(QFileInfo(metricBatch[2]).fileName() == "cell run.v1_metrics.csv",
               "batch metrics preserves dotted spaced basename");

    const QStringList fcsBatch = paths::batchFcsPaths(duplicateBatch, root.path());
    MIB_REQUIRE(fcsBatch.size() == 3, "batch FCS produced three paths");
    MIB_EXPECT(QFileInfo(fcsBatch[0]).fileName() == "sample_3.fcs",
               "batch FCS respects the paired collision");
    MIB_EXPECT(QFileInfo(fcsBatch[1]).fileName() == "sample_4.fcs",
               "batch FCS reserves earlier pair suggestions");
    MIB_EXPECT(QFileInfo(fcsBatch[2]).fileName() == "cell run.v1.fcs",
               "batch FCS preserves dotted spaced basename");

    const QStringList allBatch = paths::batchExportAllDirectoryPaths(duplicateBatch, root.path());
    MIB_REQUIRE(allBatch.size() == 3, "batch export-all produced three paths");
    MIB_EXPECT(QFileInfo(allBatch[0]).fileName() == "sample_2",
               "batch export-all respects existing folder collision");
    MIB_EXPECT(QFileInfo(allBatch[1]).fileName() == "sample_3",
               "batch export-all reserves folder suggestions within the run");
    MIB_EXPECT(QFileInfo(allBatch[2]).fileName() == "cell run.v1",
               "batch export-all preserves dotted spaced basename");

    if (mib::test::exitCode() == 0) {
        std::printf("HDF review export path policy verified\n");
    }
    return mib::test::exitCode();
}
