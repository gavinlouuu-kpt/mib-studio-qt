#include "backend/processing/ExperimentFrameBuffer.h"

#include <algorithm>

namespace backend::services {

uint64_t processedFrameBytes(const ProcessedFrame& frame)
{
    auto matBytes = [](const cv::Mat& m) -> uint64_t {
        if (m.empty()) return 0;
        // For a ROI view this over-counts the parent (which is what the view
        // keeps alive), so it stays an upper bound on retained memory.
        return static_cast<uint64_t>(m.total()) * m.elemSize();
    };
    uint64_t bytes = matBytes(frame.originalImage) + matBytes(frame.processedImage);
    for (const auto& s : frame.seriesImages) {
        // seriesImages[0] is normally the trigger image shared with originalImage.
        if (!s.empty() && s.data != frame.originalImage.data) bytes += matBytes(s);
    }
    return bytes;
}

ExperimentFrameBuffer::ExperimentFrameBuffer(const Policy& policy) : policy_(policy)
{
    if (policy_.maxFrames == 0) policy_.maxFrames = 1;
}

void ExperimentFrameBuffer::setPolicy(const Policy& policy)
{
    std::scoped_lock lk(mutex_);
    policy_ = policy;
    if (policy_.maxFrames == 0) policy_.maxFrames = 1;
}

ExperimentFrameBuffer::Policy ExperimentFrameBuffer::policy() const
{
    std::scoped_lock lk(mutex_);
    return policy_;
}

bool ExperimentFrameBuffer::overBoundLocked() const
{
    const size_t total = valid_.size() + invalid_.size();
    if (total > policy_.maxFrames) return true;
    return policy_.maxBytes > 0 && bytes_ > policy_.maxBytes;
}

void ExperimentFrameBuffer::trimLocked(size_t& droppedValid, size_t& droppedInvalid)
{
    while (overBoundLocked() && !invalid_.empty()) {
        bytes_ -= std::min(bytes_, invalid_.front().bytes);
        invalid_.pop_front();
        ++droppedInvalid;
    }
    while (overBoundLocked() && !valid_.empty()) {
        bytes_ -= std::min(bytes_, valid_.front().bytes);
        valid_.pop_front();
        ++droppedValid;
    }
}

ExperimentFrameBuffer::AppendResult ExperimentFrameBuffer::append(ProcessedFrame&& frame, bool isValid)
{
    AppendResult r;
    const uint64_t frameBytes = processedFrameBytes(frame);
    {
        std::scoped_lock lk(mutex_);
        // A tightened policy (setPolicy) applies on the next admission so its
        // evictions are reported to the caller like any other.
        trimLocked(r.droppedValid, r.droppedInvalid);
        const size_t currentTotal = valid_.size() + invalid_.size();
        const bool countFull = currentTotal >= policy_.maxFrames;
        const bool bytesFull = policy_.maxBytes > 0 && bytes_ + frameBytes > policy_.maxBytes;
        bool refuse = false;
        if (countFull || bytesFull) {
            // Make room by evicting sampled invalid frames first (oldest
            // first). If none can be evicted the newcomer is refused — a
            // valid frame never evicts another valid frame to enter, and an
            // invalid frame never evicts anything but invalid frames.
            while ((valid_.size() + invalid_.size() >= policy_.maxFrames ||
                    (policy_.maxBytes > 0 && bytes_ + frameBytes > policy_.maxBytes)) &&
                   isValid && !invalid_.empty()) {
                bytes_ -= std::min(bytes_, invalid_.front().bytes);
                invalid_.pop_front();
                ++r.droppedInvalid;
            }
            refuse = (valid_.size() + invalid_.size() >= policy_.maxFrames) ||
                     (policy_.maxBytes > 0 && bytes_ + frameBytes > policy_.maxBytes);
        }
        if (refuse) {
            if (isValid) ++r.droppedValid;
            else ++r.droppedInvalid;
        } else {
            Entry e;
            e.bytes = frameBytes;
            e.frame = std::move(frame);
            bytes_ += frameBytes;
            if (isValid) valid_.emplace_back(std::move(e));
            else invalid_.emplace_back(std::move(e));
            r.stored = true;
            trimLocked(r.droppedValid, r.droppedInvalid);
        }
        r.bufferedAfter = valid_.size() + invalid_.size();
        r.bytesAfter = bytes_;
        accountant_.set(bytes_, r.bufferedAfter);
    }
    if (r.droppedValid) droppedValidTotal_.fetch_add(r.droppedValid, std::memory_order_relaxed);
    if (r.droppedInvalid) droppedInvalidTotal_.fetch_add(r.droppedInvalid, std::memory_order_relaxed);
    if (r.dropped()) accountant_.noteEvicted(r.dropped());
    return r;
}

BufferedFrameCounts ExperimentFrameBuffer::counts() const
{
    std::scoped_lock lk(mutex_);
    return BufferedFrameCounts{valid_.size(), invalid_.size()};
}

bool ExperimentFrameBuffer::empty() const
{
    std::scoped_lock lk(mutex_);
    return valid_.empty() && invalid_.empty();
}

void ExperimentFrameBuffer::takeAll(std::vector<ProcessedFrame>& valid, std::vector<ProcessedFrame>& invalid)
{
    std::scoped_lock lk(mutex_);
    valid.clear();
    invalid.clear();
    valid.reserve(valid_.size());
    invalid.reserve(invalid_.size());
    for (auto& e : valid_) valid.emplace_back(std::move(e.frame));
    for (auto& e : invalid_) invalid.emplace_back(std::move(e.frame));
    valid_.clear();
    invalid_.clear();
    bytes_ = 0;
    accountant_.set(0, 0);
}

std::vector<ProcessedFrame> ExperimentFrameBuffer::copyValid() const
{
    std::scoped_lock lk(mutex_);
    std::vector<ProcessedFrame> out;
    out.reserve(valid_.size());
    for (const auto& e : valid_) out.push_back(e.frame);
    return out;
}

std::vector<ProcessedFrame> ExperimentFrameBuffer::copyInvalid() const
{
    std::scoped_lock lk(mutex_);
    std::vector<ProcessedFrame> out;
    out.reserve(invalid_.size());
    for (const auto& e : invalid_) out.push_back(e.frame);
    return out;
}

void ExperimentFrameBuffer::clear()
{
    std::scoped_lock lk(mutex_);
    valid_.clear();
    invalid_.clear();
    bytes_ = 0;
    accountant_.set(0, 0);
}

void ExperimentFrameBuffer::resetTotals()
{
    droppedValidTotal_.store(0, std::memory_order_relaxed);
    droppedInvalidTotal_.store(0, std::memory_order_relaxed);
    accountant_.resetEvicted();
    std::scoped_lock lk(mutex_);
    accountant_.set(bytes_, valid_.size() + invalid_.size());
}

diagnostics::MemoryOwnerStats ExperimentFrameBuffer::memoryStats() const
{
    Policy p = policy();
    return accountant_.snapshot("processing.experimentBuffer", diagnostics::MemoryKnowledge::Measured,
                                p.maxBytes, p.maxFrames,
                                "frames accepted for persistence since the last flush; evictions are "
                                "persistenceCancelledByPolicy");
}

} // namespace backend::services
