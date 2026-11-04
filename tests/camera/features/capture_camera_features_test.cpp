// E02 (E-2): GenApi feature access through CaptureService's camera guard, with
// the MockCamera's fake node map (evidence "fixture/mock", never "device"), and
// the bridge JSON documents. Feature names are discovered from the tree, never
// named here. Includes a stop/start stress for the guard (TSan lane).
#include "backend/camera/features/FeatureJson.h"
#include "backend/camera/mock/MockCamera.h"
#include "backend/services/CaptureService.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>

#include <atomic>
#include <chrono>
#include <set>
#include <string>
#include <thread>

using nlohmann::json;
namespace features = ::camera::features;

namespace {

class CapturePort final : public features::GuardedPort {
public:
    explicit CapturePort(backend::services::CaptureService& c, std::string block = {})
        : capture_(c), block_(std::move(block)) {}
    features::Access run(const std::function<void(features::Port&, features::CurrentSession&)>& fn,
                         std::string& reason) override {
        return capture_.withCameraFeatures(fn, reason, block_);
    }

private:
    backend::services::CaptureService& capture_;
    std::string block_;
};

// A camera with no GenApi node map.
class PlainCamera final : public ::camera::common::ICamera {
public:
    void applyConfig(const ::camera::common::CameraConfig&) override {}
    bool start() override { return running_ = true; }
    void stop() override { running_ = false; }
    bool isRunning() const override { return running_; }
    bool grabFrame(::camera::common::Frame&) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return running_.load();
    }
    bool pollStats(::camera::common::CameraStats&) const override { return false; }

private:
    std::atomic<bool> running_{false};
};

json parse(const std::string& text) { return json::parse(text); }

bool startAndWait(backend::services::CaptureService& c) {
    c.requestStart();
    return c.waitForState({backend::services::CaptureLifecycleState::Running,
                           backend::services::CaptureLifecycleState::Faulted},
                          std::chrono::seconds(10)) ==
           backend::services::CaptureLifecycleState::Running;
}

json request(const json& session, const std::string& feature, const json* value = nullptr) {
    json r{{"session", session}, {"module", "remote"}, {"feature", feature}};
    if (value) r["value"] = *value;
    return r;
}

} // namespace

int main() {
    mib::test::Watchdog watchdog(60);
    mib::test::TempDir dir("mib_capture_features");
    const cv::Mat frame(16, 16, CV_8UC1, cv::Scalar(90));
    MIB_REQUIRE(cv::imwrite((dir.path() / "frame_000.tiff").string(), frame), "fixture frame");

    backend::services::CaptureService capture;
    capture.setCameraFactory([&] {
        ::camera::mock::MockCameraOptions o;
        o.folder = dir.path();
        o.frameInterval = std::chrono::microseconds(2000);
        return std::make_unique<::camera::mock::MockCamera>(o);
    });
    CapturePort port(capture);

    watchdog.mark("no session");
    auto tree = parse(features::featureTreeJson(port, "{}"));
    MIB_EXPECT(tree["outcome"] == "refused" && tree["session"].is_null(),
               "no camera: tree refused, " + tree.dump());

    watchdog.mark("start 1");
    MIB_REQUIRE(startAndWait(capture), "mock capture starts");
    tree = parse(features::featureTreeJson(port, R"({"modules":["remote","stream"]})"));
    MIB_REQUIRE(tree["outcome"] == "read", tree.dump());
    MIB_EXPECT(tree["evidence"] == "fixture/mock", "mock evidence is fixture/mock");
    const auto session1 = tree["session"];
    MIB_EXPECT(session1["generation"] == std::to_string(capture.lifecycleSnapshot().generation),
               "token generation is the capture session generation");
    MIB_EXPECT(session1["capability_revision"] == "1", "revision starts at 1");
    const auto& remote = tree["modules"][0];
    MIB_EXPECT(remote["module"] == "remote" && remote["enumerated"] == true, "remote enumerated");
    MIB_EXPECT(!remote["roots"].empty() && !remote["categories"].empty(), "categories present");
    // The mock's node map has no stream module: absent, never an empty enumerated tree.
    const auto& stream = tree["modules"][1];
    MIB_EXPECT(stream["module"] == "stream" && stream["enumerated"] == false &&
                   stream["categories"].empty() && stream["features"].empty() &&
                   stream["reason"].get<std::string>().rfind("Absent: ", 0) == 0,
               "an absent module is reported absent: " + stream.dump());

    // Discover a writable float and a read-only feature from the tree.
    std::string writableFloat, readOnly;
    json floatMax;
    for (const auto& f : remote["features"]) {
        const auto& d = f["descriptor"];
        if (d.is_null()) continue;
        if (writableFloat.empty() && d["kind"] == "float" && d["writable"] == true) {
            writableFloat = f["name"];
            floatMax = d["maximum"]["value"];
        }
        if (readOnly.empty() && d["readable"] == true && d["writable"] == false) readOnly = f["name"];
    }
    MIB_REQUIRE(!writableFloat.empty() && !readOnly.empty(), "fixture has both shapes");

    watchdog.mark("read/write");
    auto read = parse(features::featureReadJson(port, request(session1, writableFloat).dump()));
    MIB_EXPECT(read["outcome"] == "read" && read["readback"]["writable"] == true, read.dump());
    const double target = floatMax.get<double>() / 2;
    const json value{{"type", "float"}, {"value", target}};
    auto written = parse(features::featureWriteJson(port, request(session1, writableFloat, &value).dump()));
    MIB_EXPECT(written["outcome"] == "readback", written.dump());
    MIB_EXPECT(written["requested"]["value"] == target && written["readback"]["value"]["value"] == target,
               "requested and readback both reported");
    MIB_EXPECT(written["session"]["capability_revision"] == "2", "write advances the revision");
    const auto session2 = written["session"];

    auto stale = parse(features::featureWriteJson(port, request(session1, writableFloat, &value).dump()));
    MIB_EXPECT(stale["outcome"] == "stale", "pre-write token refused: " + stale.dump());

    const json roValue{{"type", "string"}, {"value", "x"}};
    auto refused = parse(features::featureWriteJson(port, request(session2, readOnly, &roValue).dump()));
    MIB_EXPECT(refused["outcome"] == "refused" && refused["readback"]["writable"] == false,
               "read-only refused: " + refused.dump());

    CapturePort blocked(capture, "recording");
    auto block = parse(features::featureWriteJson(blocked, request(session2, writableFloat, &value).dump()));
    MIB_EXPECT(block["outcome"] == "refused" && block["reason"] == "recording", "write gate");

    // The gate is evaluated under the camera guard: refuseAll refuses every
    // operation before the port, refuseWrites only writes.
    {
        int evaluations = 0;
        backend::services::CameraFeatureGate gate;
        gate.refuseAll = [&] {
            ++evaluations;
            return std::string("run in progress");
        };
        struct GatedPort final : features::GuardedPort {
            backend::services::CaptureService& c;
            const backend::services::CameraFeatureGate& g;
            bool touched{false};
            GatedPort(backend::services::CaptureService& cs, const backend::services::CameraFeatureGate& gg)
                : c(cs), g(gg) {}
            features::Access run(const std::function<void(features::Port&, features::CurrentSession&)>& fn,
                                 std::string& reason) override {
                return c.withCameraFeatures(
                    [&](features::Port& p, features::CurrentSession& s) {
                        touched = true;
                        fn(p, s);
                    },
                    reason, g);
            }
        } gated(capture, gate);
        auto t = parse(features::featureTreeJson(gated, "{}"));
        auto r = parse(features::featureReadJson(gated, request(session2, writableFloat).dump()));
        auto w = parse(features::featureWriteJson(gated, request(session2, writableFloat, &value).dump()));
        MIB_EXPECT(t["outcome"] == "refused" && t["reason"] == "run in progress" && t["modules"].empty(),
                   "refuseAll refuses the tree: " + t.dump());
        MIB_EXPECT(r["outcome"] == "refused" && r["reason"] == "run in progress" && r["readback"].is_null(),
                   "refuseAll refuses a read: " + r.dump());
        MIB_EXPECT(w["outcome"] == "refused" && w["reason"] == "run in progress" && w["readback"].is_null(),
                   "refuseAll refuses a write: " + w.dump());
        MIB_EXPECT(!gated.touched && evaluations == 3, "the port is never touched; one decision per operation");
        gate.refuseAll = {};
        gate.refuseWrites = [] { return std::string("realtime"); };
        r = parse(features::featureReadJson(gated, request(session2, writableFloat).dump()));
        w = parse(features::featureWriteJson(gated, request(session2, writableFloat, &value).dump()));
        MIB_EXPECT(r["outcome"] == "read", "refuseWrites leaves reads open: " + r.dump());
        MIB_EXPECT(w["outcome"] == "refused" && w["reason"] == "realtime" && w["session"] == session2,
                   "refuseWrites refuses the write without advancing the revision: " + w.dump());
    }

    auto malformed = parse(features::featureWriteJson(port, R"({"session":{"source":"x"}})"));
    MIB_EXPECT(malformed["outcome"] == "refused", "malformed request refused before the port");

    watchdog.mark("restart");
    capture.stop();
    auto closed = parse(features::featureReadJson(port, request(session2, writableFloat).dump()));
    MIB_EXPECT(closed["outcome"] == "stale", "closed session: stale, " + closed.dump());
    MIB_REQUIRE(startAndWait(capture), "mock capture restarts");
    auto restarted = parse(features::featureReadJson(port, request(session2, writableFloat).dump()));
    MIB_EXPECT(restarted["outcome"] == "stale" &&
                   restarted["session"]["generation"] != session2["generation"],
               "older generation refused after restart: " + restarted.dump());

    // Guard stress: readers race stop/start; every reply is one of the
    // documented outcomes and nothing crashes or deadlocks.
    watchdog.mark("stress");
    std::atomic<bool> done{false};
    std::atomic<int> replies{0}, bad{0};
    std::thread reader([&] {
        const std::set<std::string> allowed{"read", "stale", "refused"};
        while (!done.load()) {
            const auto t = parse(features::featureTreeJson(port, R"({"modules":["remote"]})"));
            if (!allowed.count(t["outcome"].get<std::string>())) ++bad;
            ++replies;
        }
    });
    for (int i = 0; i < 6; ++i) {
        watchdog.mark("stress cycle");
        capture.stop();
        MIB_REQUIRE(startAndWait(capture), "restart under load");
    }
    done = true;
    reader.join();
    MIB_EXPECT(bad.load() == 0 && replies.load() > 0, "only documented outcomes under races");
    capture.stop();

    // E01 (E-4): a backend restart. A new CaptureService (as a new backend
    // process constructs) restarts generations at 1, so its first token can
    // equal session1 in source, generation and revision. The instance differs,
    // and the earlier instance's token is refused as stale.
    watchdog.mark("backend restart");
    {
        backend::services::CaptureService reborn;
        reborn.setCameraFactory([&] {
            ::camera::mock::MockCameraOptions o;
            o.folder = dir.path();
            o.frameInterval = std::chrono::microseconds(2000);
            return std::make_unique<::camera::mock::MockCamera>(o);
        });
        MIB_EXPECT(!capture.instanceId().empty() && reborn.instanceId() != capture.instanceId(),
                   "every backend instance draws its own instance id");
        MIB_REQUIRE(startAndWait(reborn), "the new instance's capture starts");
        CapturePort rebornPort(reborn);
        const auto fresh = parse(features::featureTreeJson(rebornPort, R"({"modules":["remote"]})"));
        MIB_REQUIRE(fresh["outcome"] == "read", fresh.dump());
        const auto& current = fresh["session"];
        MIB_EXPECT(current["source"] == session1["source"] &&
                       current["generation"] == session1["generation"] &&
                       current["capability_revision"] == session1["capability_revision"],
                   "the restart reuses source, generation and revision: " + current.dump() +
                       " vs " + session1.dump());
        MIB_EXPECT(current["instance"] == reborn.instanceId() &&
                       session1["instance"] == capture.instanceId(),
                   "each token names its backend instance");
        auto old = parse(features::featureReadJson(rebornPort, request(session1, writableFloat).dump()));
        MIB_EXPECT(old["outcome"] == "stale" && old["session"] == current,
                   "a token from before the restart is stale: " + old.dump());
        auto oldWrite =
            parse(features::featureWriteJson(rebornPort, request(session1, writableFloat, &value).dump()));
        MIB_EXPECT(oldWrite["outcome"] == "stale" && oldWrite["readback"].is_null(),
                   "a write under the old token never reaches the port: " + oldWrite.dump());
        auto now = parse(features::featureReadJson(rebornPort, request(current, writableFloat).dump()));
        MIB_EXPECT(now["outcome"] == "read", "the new instance's token reads: " + now.dump());
        auto missing = session1;
        missing.erase("instance");
        auto bare = parse(features::featureReadJson(rebornPort, request(missing, writableFloat).dump()));
        MIB_EXPECT(bare["outcome"] == "refused", "a token without instance is malformed: " + bare.dump());
        reborn.stop();
    }

    // A camera without GenApi reports unsupported, not an empty tree.
    watchdog.mark("unsupported");
    backend::services::CaptureService plain;
    plain.setCameraFactory([] { return std::make_unique<PlainCamera>(); });
    MIB_REQUIRE(startAndWait(plain), "plain camera starts");
    CapturePort plainPort(plain);
    tree = parse(features::featureTreeJson(plainPort, "{}"));
    MIB_EXPECT(tree["outcome"] == "unsupported" && tree["modules"].empty(), tree.dump());
    read = parse(features::featureReadJson(plainPort, request(session1, writableFloat).dump()));
    MIB_EXPECT(read["outcome"] == "unsupported", read.dump());
    plain.stop();
    return mib::test::exitCode();
}
