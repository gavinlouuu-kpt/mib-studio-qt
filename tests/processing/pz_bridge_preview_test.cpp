// Whole-frame Align previews from the PZ7035 results bridge (#501 P1): the pzres preview handshake
// (pz7035-imx426 tools/pzres/pzres.c, docs/YOFO_HOST_INTERFACE.md) against a fake bridge, the
// camera that serves them to CaptureService, and the expected-core feature that gates them.
#include "backend/processing/pz/PzBridgePreview.h"
#include "backend/pz/PzBridgePreviewCamera.h"
#include "backend/pz/PzPlatformMonitor.h"
#include "pz_mib_abi.h"

#include "support/assert.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ppz = backend::processing::pz;

namespace {

struct Write {
    uint32_t off, value;
};

class FakeBridge final : public ppz::IPzBridgeIo {
public:
    std::map<uint32_t, uint32_t> regs;
    std::vector<Write> writes;
    std::vector<uint8_t> slots;
    bool holdSticks{true};
    uint32_t reg(uint32_t off) override {
        if (off == PZ_MIB_REG_PREVIEW_HOLD && !holdSticks) return 0;
        auto it = regs.find(off);
        return it == regs.end() ? 0 : it->second;
    }
    void setReg(uint32_t off, uint32_t v) override {
        writes.push_back({off, v});
        regs[off] = v;
        if (off == PZ_MIB_REG_SNAPSHOT) regs[PZ_MIB_REG_SNAPSHOT_SEQ] += 1;
    }
    void readPreview(size_t offset, uint8_t* dst, size_t n) override {
        for (size_t i = 0; i < n; ++i) dst[i] = slots[offset + i];
    }
    void sleepUs(unsigned) override {}
    size_t indexOf(uint32_t off, uint32_t value) const {
        for (size_t i = 0; i < writes.size(); ++i)
            if (writes[i].off == off && writes[i].value == value) return i;
        return SIZE_MAX;
    }
    // Slot `s` (0-based) READY with frame id `fid`, filled with `fill`.
    void publish(uint32_t s, uint32_t fid, uint8_t fill, const ppz::BridgePreviewConfig& c) {
        regs[PZ_MIB_REG_PREVIEW_RING_HEAD] = s + 1;
        regs[PZ_MIB_REG_PREVIEW_SLOT_FRAME + 8 * s] = fid;
        std::fill(slots.begin() + s * c.slotBytes(), slots.begin() + (s + 1) * c.slotBytes(), fill);
    }
};

void testArmSequence() {
    FakeBridge b;
    b.regs[PZ_MIB_REG_EPOCH] = 6;
    b.regs[PZ_MIB_REG_RESULT_RING_HEAD] = 4096;
    ppz::BridgePreviewConfig c;
    std::string err;
    MIB_REQUIRE(ppz::armBridgePreview(b, c, &err), "arm: " + err);
    const auto stop = b.indexOf(PZ_MIB_REG_CONTROL, PZ_MIB_CONTROL_STOP);
    const auto geometry = b.indexOf(PZ_MIB_REG_CONFIG_SHADOW, (624u << 16) | 816u);
    const auto commit = b.indexOf(PZ_MIB_REG_CONFIG_COMMIT, 7);
    const auto arm = b.indexOf(PZ_MIB_REG_CONTROL, PZ_MIB_CONTROL_ARM);
    MIB_EXPECT(stop == 0, "STOP first (geometry needs a drained bridge)");
    MIB_EXPECT(stop < geometry && geometry < commit && commit < arm && arm == b.writes.size() - 1,
               "geometry, commit EPOCH+1, then ARM last");
    MIB_EXPECT(b.regs[PZ_MIB_REG_CONFIG_SHADOW + 4 * PZ_MIB_CONFIG_PREVIEW_DECIMATION] == 40 &&
                   b.regs[PZ_MIB_REG_CONFIG_SHADOW + 4 * PZ_MIB_CONFIG_PIXEL_FORMAT] == PZ_MIB_PIXEL_FORMAT_MONO8,
               "Mono8, every 40th frame");
    MIB_EXPECT(b.regs[PZ_MIB_REG_PREVIEW_RING_BASE_LO] == 0x3F100000u && b.regs[PZ_MIB_REG_PREVIEW_RING_ENTRIES] == 2 &&
                   b.regs[PZ_MIB_REG_PREVIEW_SLOT_BYTES] == 509184u,
               "two 816x624 slots after the result ring");
    MIB_EXPECT(b.regs[PZ_MIB_REG_RESULT_RING_TAIL] == 4096, "result ring tail = head");

    ppz::BridgePreviewConfig bad = c;
    bad.width = 815;
    MIB_EXPECT(!ppz::armBridgePreview(b, bad, &err), "width must be a multiple of 8");
}

void testPolling() {
    FakeBridge b;
    ppz::BridgePreviewConfig c;
    b.slots.assign(2 * c.slotBytes(), 0);
    b.regs[PZ_MIB_REG_STATE] = PZ_MIB_STATE_RUNNING;
    ppz::BridgePreviewImage img;
    bool lost = true;
    MIB_EXPECT(!ppz::pollBridgePreview(b, c, 0, img, &lost) && !lost, "no READY slot yet: wait, bridge fine");

    b.regs[PZ_MIB_REG_RESULT_RING_HEAD] = 777;
    b.publish(1, 4000, 0x5A, c);
    MIB_REQUIRE(ppz::pollBridgePreview(b, c, 0, img, &lost), "newest READY slot copied");
    MIB_EXPECT(img.frameId == 4000 && img.width == 816 && img.height == 624 && img.pixels.size() == 509184 &&
                   img.pixels.front() == 0x5A && img.pixels.back() == 0x5A,
               "whole frame of slot 1 at the configured geometry");
    MIB_EXPECT(b.indexOf(PZ_MIB_REG_PREVIEW_HOLD, 2) != SIZE_MAX && b.regs[PZ_MIB_REG_PREVIEW_HOLD] == 0,
               "slot held while copied, released after");
    MIB_EXPECT(b.regs[PZ_MIB_REG_RESULT_RING_TAIL] == 777, "result ring released on every poll");
    MIB_EXPECT(!ppz::pollBridgePreview(b, c, 4000, img, &lost), "the same frame is never sent twice");

    b.publish(0, 4040, 0x11, c);
    b.holdSticks = false;
    MIB_EXPECT(!ppz::pollBridgePreview(b, c, 4000, img, &lost) && !lost, "HOLD lost to the writer: retry, no error");
    b.holdSticks = true;
    MIB_EXPECT(ppz::pollBridgePreview(b, c, 4000, img, &lost) && img.frameId == 4040 && img.pixels[0] == 0x11,
               "next poll takes it");

    b.regs[PZ_MIB_REG_STATE] = PZ_MIB_STATE_FAULT;
    MIB_EXPECT(!ppz::pollBridgePreview(b, c, 4040, img, &lost) && lost, "a faulted bridge is reported");
    std::string err;
    b.regs[PZ_MIB_REG_COUNTER + 4 * PZ_MIB_COUNTER_PREVIEW_DROPPED] = 3;
    MIB_EXPECT(!ppz::waitBridgePreview(b, c, 4040, std::chrono::milliseconds(50), img, &err) &&
                   err.find("dropped 3") != std::string::npos && err.find("left ARMED/RUNNING") != std::string::npos,
               "the failure names the bridge state and preview counters: " + err);

    b.regs[PZ_MIB_REG_STATE] = PZ_MIB_STATE_RUNNING;
    MIB_EXPECT(!ppz::waitBridgePreview(b, c, 4040, std::chrono::milliseconds(20), img, &err) &&
                   err.find("no new preview published") != std::string::npos,
               "a timeout says no preview was published");

    ppz::stopBridgePreview(b);
    MIB_EXPECT(b.writes.back().off == PZ_MIB_REG_CONTROL && b.writes.back().value == PZ_MIB_CONTROL_STOP, "STOP on leave");
}

// A provider that serves previews from a FakeBridge, as PzDevMemExecutionProvider does.
class PreviewProvider final : public backend::processing::IExecutionProvider {
public:
    FakeBridge bridge;
    int starts{0}, stops{0};
    bool refuseStart{false};
    std::string name() const override { return "fake-preview"; }
    void setSink(Sink) override {}
    bool configure(const ppz::CompiledProfile&, std::string*) override { return true; }
    bool start(uint64_t, std::string*) override { return true; }
    void stop() override {}
    backend::processing::ProviderStatus status() const override { return {}; }
    bool startPreview(const ppz::BridgePreviewConfig& c, std::string* error) override {
        if (refuseStart) {
            if (error) *error = "PL not configured";
            return false;
        }
        ++starts;
        config = c;
        bridge.slots.assign(2 * c.slotBytes(), 0);
        bridge.regs[PZ_MIB_REG_STATE] = PZ_MIB_STATE_RUNNING;
        return ppz::armBridgePreview(bridge, c, error);
    }
    bool fetchPreview(uint64_t last, std::chrono::milliseconds timeout, ppz::BridgePreviewImage& out,
                      std::string* error) override {
        return ppz::waitBridgePreview(bridge, config, last, timeout, out, error);
    }
    void stopPreview() override { ++stops; }
    ppz::BridgePreviewConfig config;
};

void testCamera() {
    PreviewProvider p;
    backend::pz::PzBridgePreviewCamera cam(p, ppz::BridgePreviewConfig{}, std::chrono::milliseconds(5), 3);
    MIB_REQUIRE(cam.start() && cam.isRunning() && p.starts == 1, "start arms the bridge preview");
    camera::common::Frame f;
    p.bridge.publish(0, 100, 7, p.config);
    MIB_REQUIRE(cam.grabFrame(f), "a whole frame");
    MIB_EXPECT(f.width == 816 && f.height == 624 && f.linePitch == 816 && f.pixelFormat == 0x01080001u &&
                   f.data.size() == 509184 && f.data[0] == 7 && f.timestamp == 100,
               "Mono8 816x624, stamped with the sensor frame id");
    MIB_EXPECT(!cam.grabFrame(f) && cam.isRunning(), "one timeout is tolerated");
    p.bridge.publish(1, 140, 9, p.config);
    MIB_EXPECT(cam.grabFrame(f) && f.data[0] == 9, "the next frame resets the timeout count");
    MIB_EXPECT(!cam.grabFrame(f) && !cam.grabFrame(f) && cam.isRunning(), "two timeouts tolerated");
    MIB_EXPECT(!cam.grabFrame(f) && !cam.isRunning() && cam.lastFailure().code == "pz.preview_timeout" &&
                   cam.lastFailure().message.find("no new preview") != std::string::npos,
               "the third in a row stops the camera with the reason");
    MIB_EXPECT(p.stops == 1, "and stops the bridge preview");

    MIB_REQUIRE(cam.start(), "restart");
    p.bridge.regs[PZ_MIB_REG_PREVIEW_RING_HEAD] = 0; // nothing READY after the re-arm
    p.bridge.regs[PZ_MIB_REG_STATE] = PZ_MIB_STATE_FAULT;
    MIB_EXPECT(!cam.grabFrame(f) && !cam.isRunning() && cam.lastFailure().code == "pz.preview_lost",
               "a faulted bridge stops at once");

    // Before the first frame the sensor may still be powering up: a longer grace applies.
    PreviewProvider cold;
    backend::pz::PzBridgePreviewCamera warming(cold, ppz::BridgePreviewConfig{}, std::chrono::milliseconds(2), 2, 4);
    MIB_REQUIRE(warming.start(), "cold start");
    MIB_EXPECT(!warming.grabFrame(f) && !warming.grabFrame(f) && !warming.grabFrame(f) && warming.isRunning(),
               "three timeouts before the first frame are tolerated (startup grace 4)");
    cold.bridge.publish(0, 10, 3, cold.config);
    MIB_EXPECT(warming.grabFrame(f) && f.data[0] == 3, "the first frame arrives late");
    MIB_EXPECT(!warming.grabFrame(f) && !warming.grabFrame(f) && !warming.isRunning(),
               "after it the normal limit (2) applies");

    PreviewProvider refusing;
    refusing.refuseStart = true;
    backend::pz::PzBridgePreviewCamera blocked(refusing, ppz::BridgePreviewConfig{});
    MIB_EXPECT(!blocked.start() && blocked.lastFailure().message.find("PL not configured") != std::string::npos,
               "a refused start reports why");
}

void testExpectedCoreFeatures() {
    std::string err;
    const auto r8 = backend::pz::parseExpectedCore(
        R"({"build_id":"a781ec5a6ca49a2e54f9fb5dbab25315","profile_id":"eea09a3f9cbf552c749fa66e205ef961",)"
        R"("features":["align_whole_frame_preview"]})", &err);
    MIB_REQUIRE(r8, "results8 core.json parses: " + err);
    MIB_EXPECT(r8->has(backend::pz::kFeatureAlignWholeFrame), "results8 has whole-frame Align");
    const auto r6 = backend::pz::parseExpectedCore(
        R"({"build_id":"76aea7655189e35b1b629289f363df5e","features":[]})", &err);
    MIB_EXPECT(r6 && !r6->has(backend::pz::kFeatureAlignWholeFrame), "results6 does not");
    const auto old = backend::pz::parseExpectedCore(R"({"build_id":"76aea7655189e35b1b629289f363df5e"})", &err);
    MIB_EXPECT(old && old->features.empty(), "older files without features still parse");
}

} // namespace

int main() {
    testArmSequence();
    testPolling();
    testCamera();
    testExpectedCoreFeatures();
    return mib::test::exitCode();
}
