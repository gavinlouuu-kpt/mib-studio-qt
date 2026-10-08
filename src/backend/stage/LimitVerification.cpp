#include "backend/stage/LimitVerification.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <thread>

namespace backend::stage {

namespace {

using Clock = std::chrono::steady_clock;

void say(const LimitVerificationOptions& o, const std::string& text)
{
    SPDLOG_INFO("verifyLimits: {}", text);
    if (o.report) o.report(text);
}

bool isCancelled(const LimitVerificationOptions& o) { return o.cancelled && o.cancelled(); }

// Waits for the axis to stop; stops it on cancel, deadline, e-stop or alarm.
StageError waitIdle(IMotionStage& stage, const LimitVerificationOptions& o, Clock::time_point deadline,
                    StageStatus& status, LimitVerificationResult& result)
{
    for (;;) {
        if (isCancelled(o)) {
            stage.stop();
            result.cancelled = true;
            return StageError::None;
        }
        const StageError err = stage.readStatus(status);
        if (err != StageError::None) {
            stage.stop();
            return err;
        }
        if (status.emergencyStop) return StageError::EmergencyStop;
        if (status.driverAlarm) return StageError::DriverAlarm;
        if (status.state != MoveState::Moving) return StageError::None;
        if (Clock::now() > deadline) {
            stage.stop();
            result.detail = "the move did not finish before its deadline";
            return StageError::Timeout;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(o.pollMs));
    }
}

Clock::time_point deadlineFor(double distanceUm, const LimitVerificationOptions& o)
{
    const double seconds = distanceUm / o.searchSpeedUmS * 2.0 + 5.0;
    return Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
}

// Operator-paced search toward one switch: confirm, then controller-bounded
// steps until exactly that switch trips or the travel cap is reached.
StageError search(IMotionStage& stage, const LimitVerificationOptions& o, Direction direction, double& positionUm,
                  LimitVerificationResult& result)
{
    const bool negative = direction == Direction::Negative;
    const char* side = negative ? "negative" : "positive";
    const char* other = negative ? "positive" : "negative";
    StageStatus status;
    StageError err = stage.readStatus(status);
    if (err != StageError::None) return err;
    const double cap = std::floor(o.maxTravelUm);
    const double step = std::max(1.0, std::floor(std::min(o.stepUm, cap)));
    const double start = status.positionUm;

    if (!(negative ? status.limitNegative : status.limitPositive)) {
        const std::string prompt = std::string("At ") + std::to_string(std::lround(start)) + " um. Next: move toward the " +
                                   side + " limit switch in steps of " + std::to_string(static_cast<int>(step)) +
                                   " um at " + std::to_string(static_cast<int>(o.searchSpeedUmS)) + " um/s, at most " +
                                   std::to_string(static_cast<int>(cap)) + " um in total. Watch the stage.";
        if (!o.confirm || !o.confirm(prompt)) {
            result.cancelled = true;
            result.detail = std::string("operator did not confirm the move toward the ") + side + " switch";
            return StageError::None;
        }
        double travelled = 0.0;
        while (!(negative ? status.limitNegative : status.limitPositive)) {
            // The opposite switch tripping after we moved means swapped wiring.
            const bool wrongSwitch = negative ? status.limitPositive : status.limitNegative;
            if (wrongSwitch && travelled > 0.0) break;
            const double remaining = cap - travelled;
            if (remaining < 1.0) break;
            const double move = std::min(step, std::floor(remaining));
            if (isCancelled(o)) {
                stage.stop();
                result.cancelled = true;
                return StageError::None;
            }
            const double before = status.positionUm;
            if ((err = stage.moveRelative(negative ? -move : move)) != StageError::None) return err;
            if ((err = waitIdle(stage, o, deadlineFor(move, o), status, result)) != StageError::None) return err;
            if (result.cancelled) return StageError::None;
            travelled = std::abs(status.positionUm - start);
            say(o, std::string("position ") + std::to_string(std::lround(status.positionUm)) + " um, " +
                       std::to_string(std::lround(travelled)) + " of at most " + std::to_string(static_cast<int>(cap)) +
                       " um toward the " + side + " switch");
            const bool switchActive = status.limitNegative || status.limitPositive;
            if (!switchActive && std::abs(status.positionUm - before) < move - 1.0) {
                result.detail = "the axis stopped short of its step without a limit switch (obstruction or stall?)";
                return StageError::LimitCheckFailed;
            }
        }
    }
    const bool expected = negative ? status.limitNegative : status.limitPositive;
    const bool unexpected = negative ? status.limitPositive : status.limitNegative;
    if (unexpected) {
        result.detail = std::string("the ") + other + " switch is active at the " + side +
                        " end: switches swapped or shorted";
        return StageError::LimitCheckFailed;
    }
    if (!expected) {
        result.detail = std::string("no ") + side + " limit switch within " + std::to_string(static_cast<int>(cap)) +
                        " um: check the limit-switch wiring";
        return StageError::LimitCheckFailed;
    }
    positionUm = status.positionUm;
    say(o, std::string("the ") + side + " switch tripped at " + std::to_string(std::lround(positionUm)) + " um");
    return StageError::None;
}

} // namespace

LimitsVerificationStore::LimitsVerificationStore(std::string path) : path_(std::move(path)) {}

std::optional<LimitsVerifiedRecord> LimitsVerificationStore::find(const std::string& serial) const
{
    std::ifstream in(path_);
    if (!in || serial.empty()) return std::nullopt;
    try {
        const auto j = nlohmann::json::parse(in);
        const auto& controllers = j.at("controllers");
        if (!controllers.contains(serial)) return std::nullopt;
        const auto& c = controllers.at(serial);
        LimitsVerifiedRecord r;
        r.controllerSerial = serial;
        r.verifiedAtUtc = c.at("verified_at").get<std::string>();
        r.negativeUm = c.at("negative_um").get<double>();
        r.positiveUm = c.at("positive_um").get<double>();
        r.spanUm = c.at("span_um").get<double>();
        r.procedure = c.at("procedure").get<std::string>();
        return r;
    } catch (const std::exception& e) {
        SPDLOG_WARN("LimitsVerificationStore: unreadable {}: {}", path_, e.what());
        return std::nullopt;
    }
}

bool LimitsVerificationStore::save(const LimitsVerifiedRecord& r)
{
    nlohmann::json j = nlohmann::json::object();
    {
        std::ifstream in(path_);
        if (in) {
            try {
                j = nlohmann::json::parse(in);
            } catch (...) {
                j = nlohmann::json::object();
            }
        }
    }
    if (!j.contains("controllers") || !j.at("controllers").is_object()) j["controllers"] = nlohmann::json::object();
    j["controllers"][r.controllerSerial] = {{"verified_at", r.verifiedAtUtc}, {"negative_um", r.negativeUm},
                                            {"positive_um", r.positiveUm},   {"span_um", r.spanUm},
                                            {"procedure", r.procedure}};
    std::error_code ec;
    const auto parent = std::filesystem::path(path_).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    const std::string tmp = path_ + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << j.dump(2);
        if (!out) return false;
    }
    std::filesystem::rename(tmp, path_, ec);
    return !ec;
}

LimitVerificationResult verifyLimits(IMotionStage& stage, const LimitVerificationOptions& o)
{
    LimitVerificationResult result;
    if (!stage.isConnected()) {
        result.error = StageError::NotConnected;
        return result;
    }
    if (!stage.isConfigured()) {
        result.error = StageError::Misconfigured;
        result.detail = "apply the stage profile first";
        return result;
    }
    StageStatus status;
    if ((result.error = stage.readStatus(status)) != StageError::None) return result;
    if (status.emergencyStop || status.driverAlarm) {
        result.error = status.emergencyStop ? StageError::EmergencyStop : StageError::DriverAlarm;
        return result;
    }
    if (status.limitNegative && status.limitPositive) {
        result.error = StageError::LimitCheckFailed;
        result.detail = "both limit switches are active: wiring fault";
        return result;
    }
    const double start = std::round(status.positionUm);
    if ((result.error = stage.setSpeed(o.searchSpeedUmS, o.accelUmS2)) != StageError::None) return result;

    const auto fail = [&](StageError err) {
        stage.stop();
        result.error = err;
        if (result.detail.empty()) result.detail = toString(err);
        SPDLOG_WARN("verifyLimits: failed: {}", result.detail);
        return result;
    };
    StageError err = search(stage, o, Direction::Negative, result.negativeUm, result);
    if (err != StageError::None) return fail(err);
    if (result.cancelled) {
        stage.stop();
        return result;
    }
    err = search(stage, o, Direction::Positive, result.positiveUm, result);
    if (err != StageError::None) return fail(err);
    if (result.cancelled) {
        stage.stop();
        return result;
    }

    result.spanUm = result.positiveUm - result.negativeUm;
    if (std::abs(result.spanUm - o.expectedSpanUm) > o.spanToleranceUm) {
        result.detail = "measured span " + std::to_string(std::lround(result.spanUm)) + " um is outside " +
                        std::to_string(static_cast<int>(o.expectedSpanUm)) + " +/- " +
                        std::to_string(static_cast<int>(o.spanToleranceUm)) + " um";
        return fail(StageError::LimitCheckFailed);
    }

    // Back to where the operator left it (inside the switches just found).
    const double back = std::min(std::max(start, result.negativeUm + 1.0), result.positiveUm - 1.0);
    say(o, "returning to the start position (" + std::to_string(static_cast<long long>(back)) + " um)");
    if ((err = stage.moveAbsolute(std::round(back))) != StageError::None) return fail(err);
    if ((err = waitIdle(stage, o, deadlineFor(result.spanUm, o), status, result)) != StageError::None) return fail(err);
    if (result.cancelled) return result;
    result.returnedToStart = std::abs(status.positionUm - std::round(back)) <= 1.0;
    say(o, "limit switches verified: span " + std::to_string(std::lround(result.spanUm)) + " um");
    return result;
}

} // namespace backend::stage
