#include "frontend/models/HdfMetricsModel.h"
#include "backend/processing/ProcessingService.h"
#include "support/assert.h"
#include <limits>
int main() {
    frontend::HdfMetricsModel model;
    std::vector<backend::services::ProcessedFrame> frames(2);
    frames[0].validation.youngsModulus = 12.345;
    frames[1].validation.youngsModulus = std::numeric_limits<double>::quiet_NaN();
    model.setSource(&frames);
    MIB_EXPECT(model.columnCount() == 24, "modulus appended as column 24");
    MIB_EXPECT(model.headerData(22, Qt::Horizontal).toString() == "Target Group",
               "existing last column unchanged");
    MIB_EXPECT(model.headerData(23, Qt::Horizontal).toString() == "Young's modulus (kPa)",
               "modulus header");
    MIB_EXPECT(model.data(model.index(0, 23)).toString() == "12.345", "finite modulus");
    MIB_EXPECT(model.data(model.index(1, 23)).toString().isEmpty(), "NaN is blank");
    return mib::test::exitCode();
}
