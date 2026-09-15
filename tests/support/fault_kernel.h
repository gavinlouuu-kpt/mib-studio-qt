// Scriptable IProcessingKernel wrapper (issue #367 tests). Delegates to the
// bundled kernel but can be told to fail, throw, or delay the empty check /
// mask generation on selected frames so tests can prove a processing failure
// is never reported as a valid empty frame.
#pragma once

#include "backend/processing/IProcessingKernel.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace mib::test {

class FaultKernel : public backend::processing::IProcessingKernel {
public:
    enum class Mode { Passthrough, FailEmptyCheck, ThrowEmptyCheck, FailMask };

    FaultKernel() : inner_(backend::processing::makeBundledProcessingKernel()) {}

    // Every `period`-th call (1-based) misbehaves; 0 = never. Set by tests.
    std::atomic<int> failPeriod{0};
    std::atomic<int> mode{static_cast<int>(Mode::Passthrough)};
    std::atomic<int> delayMs{0};
    std::atomic<uint64_t> emptyCalls{0};
    std::atomic<uint64_t> maskCalls{0};
    std::atomic<uint64_t> injectedFailures{0};

    const backend::processing::ProcessingCoreIdentity& identity() const noexcept override
    {
        return inner_->identity();
    }

    bool isEmpty(const cv::Mat& gray, const cv::Mat& background,
                 const backend::processing::KernelConfig& config,
                 const backend::processing::KernelRoi& roi, bool& outputIsEmpty,
                 std::string* error) override
    {
        const uint64_t n = emptyCalls.fetch_add(1) + 1;
        if (delayMs.load() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delayMs.load()));
        const int period = failPeriod.load();
        const auto m = static_cast<Mode>(mode.load());
        if (period > 0 && (n % static_cast<uint64_t>(period)) == 0) {
            if (m == Mode::FailEmptyCheck) {
                injectedFailures.fetch_add(1);
                if (error) *error = "injected empty-check failure";
                outputIsEmpty = true; // a buggy core may leave this "empty"
                return false;
            }
            if (m == Mode::ThrowEmptyCheck) {
                injectedFailures.fetch_add(1);
                throw std::runtime_error("injected empty-check exception");
            }
        }
        return inner_->isEmpty(gray, background, config, roi, outputIsEmpty, error);
    }

    bool processMask(const cv::Mat& gray, const cv::Mat& background,
                     const backend::processing::KernelConfig& config,
                     const backend::processing::KernelRoi& roi, cv::Mat& outputMask,
                     std::string* error) override
    {
        const uint64_t n = maskCalls.fetch_add(1) + 1;
        const int period = failPeriod.load();
        if (static_cast<Mode>(mode.load()) == Mode::FailMask && period > 0 &&
            (n % static_cast<uint64_t>(period)) == 0) {
            injectedFailures.fetch_add(1);
            if (error) *error = "injected mask failure";
            return false;
        }
        return inner_->processMask(gray, background, config, roi, outputMask, error);
    }

    bool reset(std::string* error) override { return inner_->reset(error); }

private:
    std::shared_ptr<backend::processing::IProcessingKernel> inner_;
};

} // namespace mib::test
