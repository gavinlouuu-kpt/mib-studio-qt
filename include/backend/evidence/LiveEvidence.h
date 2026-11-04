#pragma once
#include "backend/evidence/CommittedEventStore.h"
#include <set>
#include <limits>

namespace backend::evidence {
// Production owner of the source/revision fence. Lock order is owner -> store.
// Recursive only because store entry points acquire the same authority lease;
// no external callback is invoked under this lock.
class LiveEvidence final : private Authority {
public:
    explicit LiveEvidence(Limits limits = {}) : store_(*this, limits) {}
    class RevisionChange {
    public:
        RevisionChange(LiveEvidence& owner, const std::string& reason) : owner_(owner) {
            std::lock_guard<std::recursive_mutex> lock(owner_.mutex_);
            ++owner_.mutations_;
            owner_.invalidateLocked(reason);
        }
        ~RevisionChange() {
            std::lock_guard<std::recursive_mutex> lock(owner_.mutex_);
            --owner_.mutations_;
        }
        RevisionChange(const RevisionChange&) = delete;

    private:
        LiveEvidence& owner_;
    };
    RevisionChange revisionChange(const std::string& reason) {
        return RevisionChange(*this, reason);
    }
    Status begin(Identity identity, Provenance provenance, std::uint64_t firstFrame) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (mutations_) return Status::Stale;
        identity_ = std::move(identity);
        active_ = true;
        sealed_ = false;
        pending_.clear();
        nextFrame_ = firstFrame;
        nextBatch_ = nextEvent_ = 0;
        reason_.clear();
        const auto result = store_.beginEpoch(identity_, provenance);
        if (result != Status::Ok) invalidateLocked("could not begin evidence epoch");
        return result;
    }
    void invalidate(const std::string& reason) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        invalidateLocked(reason);
    }
    void admit(std::uint64_t frame) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (nextFrame_ == std::numeric_limits<std::uint64_t>::max()) nextFrame_ = frame;
        if (!active_ || sealed_ || frame < nextFrame_) return;
        if (pending_.size() >= 8192) {
            invalidateLocked("evidence admission capacity exhausted");
            return;
        }
        if (frame > nextFrame_) publish({}, {frame - nextFrame_, 0, 0, frame - nextFrame_, 0, 0});
        pending_.insert(frame);
        nextFrame_ = frame + 1;
    }
    // Exactly one terminal batch per admitted frame, including zero objects.
    void complete(std::uint64_t frame, std::vector<EventRow> rows, bool error = false) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!active_ || sealed_ || !pending_.erase(frame)) return;
        for (auto& row : rows) {
            row.frame = frame;
            row.ordinal = nextEvent_++;
        }
        const auto count = static_cast<std::uint64_t>(rows.size());
        publish(std::move(rows), {1, error ? 0ULL : 1ULL, error ? 1ULL : 0ULL, 0, count, 0});
    }
    // Only called after processing workers have joined. Unread tail and
    // abandoned admissions are declared losses, never fabricated empty frames.
    Status finish(std::uint64_t committedEndExclusive) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!active_) return Status::Stale;
        if (sealed_) return Status::Finalized;
        const auto lost =
            static_cast<std::uint64_t>(pending_.size()) +
            (committedEndExclusive > nextFrame_ ? committedEndExclusive - nextFrame_ : 0);
        pending_.clear();
        if (lost) publish({}, {lost, 0, 0, lost, 0, 0});
        auto lease = store_.acquireSnapshot(identity_, {0, nextBatch_});
        if (lease.status != Status::Ok) return lease.status;
        auto metadata = store_.metadata(lease.lease);
        store_.release(lease.lease);
        if (!metadata) return Status::Expired;
        const auto status = store_.finalize(identity_, metadata->accounting);
        sealed_ = status == Status::Ok;
        return status;
    }
    std::optional<Snapshot> snapshot() {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!active_) return {};
        auto lease = store_.acquireSnapshot(identity_, {0, nextBatch_});
        if (lease.status != Status::Ok) return {};
        auto result = store_.metadata(lease.lease);
        store_.release(lease.lease);
        return result;
    }
    // Bounded immutable paging is fenced even after source/settings replacement.
    LeaseResult acquireSnapshot(const Identity& expected) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!active_ || !(expected == identity_)) return {Status::Stale, 0};
        return store_.acquireSnapshot(expected, {0, nextBatch_});
    }
    Page page(const Identity& expected, std::uint64_t lease, std::size_t cursor,
              std::size_t limit) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!active_ || !(expected == identity_)) return {Status::Stale, {}, 0, false};
        auto metadata = store_.metadata(lease);
        if (!metadata || !(metadata->identity == expected)) return {Status::Expired, {}, 0, false};
        return store_.page(lease, cursor, limit);
    }
    std::optional<Snapshot> metadata(const Identity& expected, std::uint64_t lease) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!active_ || !(expected == identity_)) return {};
        auto result = store_.metadata(lease);
        if (!result || !(result->identity == expected)) return {};
        return result;
    }
    void release(std::uint64_t lease) { store_.release(lease); }
    std::string reason() const {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return reason_;
    }

private:
    struct Guard final : Lease {
        explicit Guard(std::recursive_mutex& mutex) : lock(mutex) {}
        std::unique_lock<std::recursive_mutex> lock;
    };
    std::unique_ptr<Lease> acquire(const Identity& expected) override {
        auto guard = std::make_unique<Guard>(mutex_);
        if (!active_ || !(expected == identity_)) return {};
        return guard;
    }
    void invalidateLocked(const std::string& reason) {
        active_ = false;
        pending_.clear();
        reason_ = reason;
    }
    void publish(std::vector<EventRow> rows, Accounting accounting) {
        if (!active_) return;
        const auto status =
            store_.commitBatch(identity_, {nextBatch_, std::move(rows), accounting});
        if (status == Status::Ok)
            ++nextBatch_;
        else
            invalidateLocked("evidence publication rejected");
    }
    mutable std::recursive_mutex mutex_;
    Identity identity_;
    unsigned mutations_{0};
    bool active_{false}, sealed_{false};
    std::string reason_{"realtime evidence has not started"};
    std::set<std::uint64_t> pending_;
    std::uint64_t nextFrame_{0}, nextBatch_{0}, nextEvent_{0};
    CommittedEventStore store_;
};
} // namespace backend::evidence
