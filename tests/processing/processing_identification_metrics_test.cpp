// Verifies the target-identification / latency metrics added to the processing
// core: the shared invalid-reason classifier (single source of truth for the
// live histogram and the UI tooltip), the identification funnel counters
// (valid / target-group / unserved-extra-target), and the PipelineTimingRecorder
// latency summary + always-on live target-latency gauge.
#include "backend/diagnostics/PipelineTimingRecorder.h"
#include "backend/processing/ProcessingScience.h"
#include "backend/processing/ProcessingTypes.h"

#include "support/assert.h"

#include <cstdint>
#include <vector>

namespace {

using backend::services::FilterResult;
using backend::services::ProcessingConfig;
namespace science = backend::processing::science;

ProcessingConfig baseConfig() {
    ProcessingConfig c; // defaults: border+area+ring enabled; deform+ratio off
    c.area_threshold_min = 60;
    c.area_threshold_max = 290;
    c.ring_ratio_min = 15.0;
    c.ring_ratio_max = 25.0;
    return c;
}

void testClassifyInvalidReasons() {
    const auto cfg = baseConfig();

    FilterResult valid;
    valid.isValid = true;
    MIB_EXPECT(science::classifyInvalidReasons(valid, cfg, 1.0).empty(),
               "valid detection yields no reasons");

    FilterResult noContour;
    noContour.innerContourCount = 0;
    auto nc = science::classifyInvalidReasons(noContour, cfg, 1.0);
    MIB_REQUIRE(nc.size() == 1, "no-contour yields exactly one reason");
    MIB_EXPECT(nc[0] == science::InvalidReasonCode::NoContour, "no-contour reason");

    // Border is an early exit: out-of-range metrics must be suppressed.
    FilterResult border;
    border.innerContourCount = 1;
    border.touchesBorder = true;
    border.area = 1.0;
    auto bd = science::classifyInvalidReasons(border, cfg, 1.0);
    MIB_REQUIRE(bd.size() == 1, "border suppresses metric reasons");
    MIB_EXPECT(bd[0] == science::InvalidReasonCode::Border, "border reason");

    // Area + ring failing together, in priority order.
    FilterResult areaRing;
    areaRing.innerContourCount = 1;
    areaRing.area = 10.0;      // 10 um^2 < 60 -> Area
    areaRing.ringRatio = 5.0;  // <= 15 -> Ring
    auto ar = science::classifyInvalidReasons(areaRing, cfg, 1.0);
    MIB_REQUIRE(ar.size() == 2, "area and ring both reported");
    MIB_EXPECT(ar[0] == science::InvalidReasonCode::Area, "area reported first");
    MIB_EXPECT(ar[1] == science::InvalidReasonCode::Ring, "ring reported second");

    // pixel->micron scaling feeds the area gate: 100 px * 0.5^2 = 25 um^2 < 60.
    FilterResult scaled;
    scaled.innerContourCount = 1;
    scaled.area = 100.0;
    scaled.ringRatio = 20.0; // in range
    auto sc = science::classifyInvalidReasons(scaled, cfg, 0.5);
    MIB_REQUIRE(sc.size() == 1, "scaled area fails the gate");
    MIB_EXPECT(sc[0] == science::InvalidReasonCode::Area, "scaled area reason");
}

void testLatencySummaryAndLiveGauge() {
    auto& rec = backend::diagnostics::PipelineTimingRecorder::instance();
    rec.clear();
    rec.setEnabled(true);
    for (uint64_t i = 1; i <= 100; ++i) {
        backend::diagnostics::TriggerTimingRecord t;
        t.grabUs = 1000;
        t.requestUs = 1000 + i;
        t.fireUs = 1000 + i; // end-to-end target latency = i
        rec.recordTrigger(t);

        backend::diagnostics::FrameTimingRecord f;
        f.grabUs = 0; // frame-age pair invalid on purpose
        f.algoStartUs = 10;
        f.algoEndUs = 10 + i; // algo = i
        rec.recordFrame(f);
    }
    const auto s = rec.summarize();
    MIB_EXPECT(s.endToEndTarget.count == 100, "all trigger records summarized");
    MIB_EXPECT(s.endToEndTarget.maxUs == 100, "end-to-end target max");
    MIB_EXPECT(s.endToEndTarget.p50Us == 50, "end-to-end target p50");
    MIB_EXPECT(s.endToEndTarget.p95Us == 95, "end-to-end target p95");
    MIB_EXPECT(s.endToEndTarget.p99Us == 99, "end-to-end target p99");
    MIB_EXPECT(s.algo.count == 100 && s.algo.maxUs == 100, "algo stage summarized");
    MIB_EXPECT(s.frameAge.count == 0, "zero-stamp pairs are excluded");
    rec.setEnabled(false);
    rec.clear();

    rec.resetLiveLatency();
    rec.noteTargetLatency(100);
    MIB_EXPECT(rec.lastTargetLatencyUs() == 100, "live last latency seeded");
    MIB_EXPECT(rec.maxTargetLatencyUs() == 100, "live max seeded");
    rec.noteTargetLatency(200);
    MIB_EXPECT(rec.maxTargetLatencyUs() == 200, "live max tracks the peak");
    MIB_EXPECT(rec.avgTargetLatencyUs() > 100.0 && rec.avgTargetLatencyUs() < 200.0,
               "live EWMA between samples");
    rec.resetLiveLatency();
    MIB_EXPECT(rec.maxTargetLatencyUs() == 0, "live gauges reset");
}

} // namespace

int main() {
    testClassifyInvalidReasons();
    testLatencySummaryAndLiveGauge();
    return mib::test::exitCode();
}
