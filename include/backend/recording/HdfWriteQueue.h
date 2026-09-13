// Bounded single-writer queue that decouples slow HDF5 writes from a fast
// producer (high-speed capture). A dedicated thread drains a FIFO of at most
// `slots` batches and writes each via an injected writeFn. A failed write or a
// submit when the queue is full is a FATAL, latched error: the writer stops,
// onError fires exactly once, and further submits are rejected. No HDF5/Qt
// dependency, so it is unit-testable with a mock writeFn.
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace backend::recording {

template <class Batch>
class HdfWriteQueue {
public:
    using WriteFn = std::function<bool(const Batch&)>;
    using ErrorFn = std::function<void(const std::string&)>;

    // slotCount: max batches in flight (not named `slots` — Qt defines that as a
    // macro, which would break this header when included from Qt translation units).
    HdfWriteQueue(size_t slotCount, WriteFn writeFn, ErrorFn onError)
        : slots_(slotCount == 0 ? 1 : slotCount),
          writeFn_(std::move(writeFn)),
          onError_(std::move(onError)) {
        worker_ = std::thread([this] { run(); });
    }

    ~HdfWriteQueue() { flushAndStop(); }

    HdfWriteQueue(const HdfWriteQueue&) = delete;
    HdfWriteQueue& operator=(const HdfWriteQueue&) = delete;

    // Non-blocking. Returns false if already errored or the queue is full; a
    // full queue latches a fatal overflow error and fires onError once.
    bool submit(Batch&& b) {
        std::string fireMsg;
        {
            std::unique_lock<std::mutex> lk(mu_);
            if (error_ || stopRequested_) return false;
            if (queue_.size() >= slots_) {
                fireMsg = latchErrorLocked("write queue overflow (disk too slow)");
            } else {
                queue_.push_back(std::move(b));
                cv_.notify_one();
                return true;
            }
        }
        if (!fireMsg.empty()) fireError(fireMsg);
        return false;
    }

    bool hasError() const {
        std::unique_lock<std::mutex> lk(mu_);
        return error_;
    }

    std::string error() const {
        std::unique_lock<std::mutex> lk(mu_);
        return errorMsg_;
    }

    // Drain queued batches, join the writer. Returns true iff no error occurred.
    bool flushAndStop() {
        {
            std::unique_lock<std::mutex> lk(mu_);
            stopRequested_ = true;
            cv_.notify_all();
        }
        if (worker_.joinable()) worker_.join();
        std::unique_lock<std::mutex> lk(mu_);
        return !error_;
    }

private:
    void run() {
        for (;;) {
            Batch b;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [this] { return !queue_.empty() || stopRequested_ || error_; });
                if (error_) return;
                if (queue_.empty()) {
                    if (stopRequested_) return;
                    continue;
                }
                b = std::move(queue_.front());
                queue_.pop_front();
            }

            bool ok = false;
            std::string thrown;
            try {
                ok = writeFn_(b);
            } catch (const std::exception& e) {
                ok = false;
                thrown = std::string("write threw: ") + e.what();
            } catch (...) {
                ok = false;
                thrown = "write threw unknown exception";
            }

            if (!ok) {
                std::string fireMsg;
                {
                    std::unique_lock<std::mutex> lk(mu_);
                    fireMsg = latchErrorLocked(thrown.empty() ? "HDF5 write failed" : thrown);
                }
                if (!fireMsg.empty()) fireError(fireMsg);
                return; // stop the writer on a fatal error
            }
        }
    }

    // Latches the error under lock. Returns the message to fire onError with, or
    // an empty string if onError already fired (so the caller skips it).
    std::string latchErrorLocked(const std::string& msg) {
        if (!error_) {
            error_ = true;
            errorMsg_ = msg;
        }
        if (!onErrorFired_) {
            onErrorFired_ = true;
            return errorMsg_;
        }
        return std::string();
    }

    void fireError(const std::string& msg) {
        if (onError_) onError_(msg);
    }

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Batch> queue_;
    bool stopRequested_ = false;
    bool error_ = false;
    bool onErrorFired_ = false;
    std::string errorMsg_;
    const size_t slots_;
    WriteFn writeFn_;
    ErrorFn onError_;
    std::thread worker_;
};

} // namespace backend::recording
