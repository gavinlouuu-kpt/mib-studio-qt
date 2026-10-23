#include "backend/evidence/CommittedEventStore.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <tuple>

namespace backend::evidence {
bool Identity::operator==(const Identity& b) const {
    return std::tie(source, session, run, epoch, revision, generation) ==
           std::tie(b.source, b.session, b.run, b.epoch, b.revision, b.generation);
}
bool Accounting::operator==(const Accounting& b) const {
    return std::tie(admittedFrames, completedFrames, errorFrames, droppedFrames, observedEvents,
                    missingEvents) == std::tie(b.admittedFrames, b.completedFrames, b.errorFrames,
                                               b.droppedFrames, b.observedEvents, b.missingEvents);
}
namespace {
std::uint64_t add(std::uint64_t a, std::uint64_t b) {
    if (b > std::numeric_limits<std::uint64_t>::max() - a)
        throw std::overflow_error("accounting overflow");
    return a + b;
}
Accounting sum(const Accounting& a, const Accounting& b) {
    return {add(a.admittedFrames, b.admittedFrames), add(a.completedFrames, b.completedFrames),
            add(a.errorFrames, b.errorFrames),       add(a.droppedFrames, b.droppedFrames),
            add(a.observedEvents, b.observedEvents), add(a.missingEvents, b.missingEvents)};
}
bool conserved(const Accounting& a) {
    return a.admittedFrames == add(add(a.completedFrames, a.errorFrames), a.droppedFrames);
}
bool text(const std::string& s) {
    return !s.empty() && s.size() <= 256;
}
bool optionalText(const std::optional<std::string>& s) {
    return !s || text(*s);
}
// Conservative payload charge: fixed structure overhead plus twice dynamic
// payload (including vector spare capacity after copies). Allocator overhead is
// not claimed as an exact RSS bound. Inputs are bounded before copying.
std::size_t rowBytes(const EventRow& r) {
    return 2 * (sizeof(EventRow) + r.set.size() + r.qc.size() +
                r.values.size() * sizeof(std::optional<double>));
}
std::size_t baseBytes(const Identity& id, const Provenance& p) {
    std::size_t n = sizeof(Snapshot) + 3 * sizeof(Gap) + 1024;
    for (const auto* s : {&id.source, &id.session, &id.epoch, &id.revision, &p.sourceKind,
                          &p.eventMode, &p.denominator})
        n += 2 * s->size();
    for (const auto* s :
         {&id.run, &p.fingerprint, &p.processing, &p.calibration, &p.mechanics, &p.clock})
        if (*s) n += 2 * (*s)->size();
    for (const auto& v : p.parameters)
        n += 2 * (sizeof(Parameter) + v.id.size() + v.unit.size());
    return n;
}
} // namespace
CommittedEventStore::CommittedEventStore(Authority& a, Limits l,
                                         std::function<Clock::time_point()> now)
    : authority_(a), limits_(l), now_(std::move(now)) {
    if (!now_ || !l.storeBytes || !l.leaseBytes || !l.leases || !l.pageRows ||
        l.leaseLifetime.count() <= 0 || l.leaseLifetime > std::chrono::hours(1))
        throw std::invalid_argument("invalid evidence limits");
}
Status CommittedEventStore::beginEpoch(const Identity& id, const Provenance& p) {
    auto fence = authority_.acquire(id);
    if (!fence) return Status::Stale;
    for (const auto* s : {&id.source, &id.session, &id.epoch, &id.revision, &p.sourceKind,
                          &p.eventMode, &p.denominator})
        if (!text(*s)) return Status::Invalid;
    for (const auto* s :
         {&id.run, &p.fingerprint, &p.processing, &p.calibration, &p.mechanics, &p.clock})
        if (!optionalText(*s)) return Status::Invalid;
    if (p.parameters.empty() || p.parameters.size() > 256) return Status::Invalid;
    for (std::size_t i = 0; i < p.parameters.size(); ++i) {
        if (!text(p.parameters[i].id) || !text(p.parameters[i].unit)) return Status::Invalid;
        for (std::size_t j = 0; j < i; ++j)
            if (p.parameters[i].id == p.parameters[j].id) return Status::Invalid;
    }
    auto bytes = baseBytes(id, p);
    if (bytes > limits_.storeBytes) return Status::ResourceExhausted;
    Snapshot next;
    next.identity = id;
    next.provenance = p;
    std::lock_guard<std::mutex> lock(mutex_);
    if (current_ && current_->identity == id) return Status::SequenceConflict;
    current_ = std::move(next);
    currentBytes_ = bytes;
    return Status::Ok;
}
Status CommittedEventStore::commitBatch(const Identity& id, const Batch& b) {
    auto fence = authority_.acquire(id);
    if (!fence) return Status::Stale;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!current_ || !(current_->identity == id)) return Status::Stale;
    auto& s = *current_;
    if (s.finalized) return Status::Finalized;
    if (b.sequence != s.watermark) return Status::SequenceConflict;
    if (b.sequence == std::numeric_limits<std::uint64_t>::max() || b.rows.size() > 65536)
        return Status::Invalid;
    Accounting totals;
    try {
        if (!conserved(b.accounting) ||
            b.accounting.observedEvents != add(b.rows.size(), b.accounting.missingEvents))
            return Status::Invalid;
        totals = sum(s.accounting, b.accounting);
    } catch (const std::overflow_error&) {
        return Status::Invalid;
    }
    std::size_t bytes = 0;
    for (std::size_t i = 0; i < b.rows.size(); ++i) {
        const auto& r = b.rows[i];
        if (!text(r.set) || !text(r.qc) || r.values.size() != s.provenance.parameters.size())
            return Status::Invalid;
        // Ordinal is global within this live epoch, including explicitly missing
        // events. It cannot be confused with an HDF5 row ordinal.
        if (r.ordinal != s.accounting.observedEvents + i) return Status::Invalid;
        for (const auto& v : r.values)
            if (v && !std::isfinite(*v)) return Status::Invalid;
        bytes += rowBytes(r);
    }
    // Once capacity is exhausted, retain a single explicit tail gap; never resume
    // row retention behind that gap or accumulate unbounded gap metadata.
    const bool overflow = (!s.gaps.empty() && s.gaps.back().reason == "capacity") ||
                          bytes > limits_.storeBytes - currentBytes_;
    auto gaps = s.gaps;
    if (overflow) {
        totals.missingEvents = add(totals.missingEvents, b.rows.size());
        if (!gaps.empty() && gaps.back().reason == "capacity") {
            gaps.back().batches.end = b.sequence + 1;
            gaps.back().events = add(*gaps.back().events, b.accounting.observedEvents);
        } else
            gaps.push_back({{b.sequence, b.sequence + 1}, b.accounting.observedEvents, "capacity"});
    } else if (b.accounting.missingEvents) {
        // Keep explicit publisher gaps bounded: coalesce to a conservative span,
        // retaining the exact missing-event count (not claiming every row absent).
        auto it = std::find_if(gaps.begin(), gaps.end(),
                               [](const Gap& g) { return g.reason == "publisher_missing"; });
        if (it == gaps.end())
            gaps.insert(
                gaps.begin(),
                {{b.sequence, b.sequence + 1}, b.accounting.missingEvents, "publisher_missing"});
        else {
            it->batches.end = b.sequence + 1;
            it->events = add(*it->events, b.accounting.missingEvents);
        }
    }
    if (b.accounting.errorFrames || b.accounting.droppedFrames) {
        auto it = std::find_if(gaps.begin(), gaps.end(),
                               [](const Gap& g) { return g.reason == "unobserved_frames"; });
        if (it == gaps.end())
            gaps.insert(gaps.begin(),
                        {{b.sequence, b.sequence + 1}, std::nullopt, "unobserved_frames"});
        else
            it->batches.end = b.sequence + 1;
    }
    if (!overflow) {
        s.rows.insert(s.rows.end(), b.rows.begin(), b.rows.end());
        currentBytes_ += bytes;
    }
    s.gaps = std::move(gaps);
    s.accounting = totals;
    s.eventCountKnown =
        s.eventCountKnown && !b.accounting.errorFrames && !b.accounting.droppedFrames;
    ++s.watermark;
    s.observed = {0, s.watermark};
    return Status::Ok;
}
Status CommittedEventStore::finalize(const Identity& id, const Accounting& totals) {
    auto fence = authority_.acquire(id);
    if (!fence) return Status::Stale;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!current_ || !(current_->identity == id)) return Status::Stale;
    // MissingEvents includes capacity loss, obtained from metadata by owner.
    if (!(current_->accounting == totals)) return Status::Invalid;
    current_->finalized = true;
    return Status::Ok;
}
void CommittedEventStore::expire() {
    const auto now = now_();
    for (auto it = held_.begin(); it != held_.end();) {
        if (it->second.expires <= now) {
            heldBytes_ -= it->second.bytes;
            it = held_.erase(it);
        } else
            ++it;
    }
}
LeaseResult CommittedEventStore::acquireSnapshot(const Identity& id, Coverage requested) {
    auto fence = authority_.acquire(id);
    if (!fence) return {Status::Stale, 0};
    std::lock_guard<std::mutex> lock(mutex_);
    expire();
    if (!current_ || !(current_->identity == id)) return {Status::Stale, 0};
    if (requested.begin != 0 || requested.end < current_->watermark) return {Status::Invalid, 0};
    if (held_.size() >= limits_.leases || currentBytes_ > limits_.leaseBytes - heldBytes_ ||
        nextLease_ == 0)
        return {Status::ResourceExhausted, 0};
    auto snapshot = *current_;
    snapshot.requested = requested;
    snapshot.requestedCovered = requested.end == snapshot.watermark && snapshot.gaps.empty();
    const auto handle = nextLease_;
    held_.emplace(handle, Held{std::move(snapshot), now_() + limits_.leaseLifetime, currentBytes_});
    ++nextLease_;
    heldBytes_ += currentBytes_;
    return {Status::Ok, handle};
}
std::optional<Snapshot> CommittedEventStore::metadata(std::uint64_t lease) {
    std::lock_guard<std::mutex> lock(mutex_);
    expire();
    auto it = held_.find(lease);
    if (it == held_.end()) return {};
    // Do not copy pixel/event payload just to obtain metadata.
    const auto& s = it->second.snapshot;
    Snapshot out;
    out.identity = s.identity;
    out.provenance = s.provenance;
    out.requested = s.requested;
    out.observed = s.observed;
    out.watermark = s.watermark;
    out.accounting = s.accounting;
    out.eventCountKnown = s.eventCountKnown;
    out.finalized = s.finalized;
    out.requestedCovered = s.requestedCovered;
    out.durableAcknowledged = s.durableAcknowledged;
    out.imageRetentionGuaranteed = s.imageRetentionGuaranteed;
    out.gaps = s.gaps;
    return out;
}
Page CommittedEventStore::page(std::uint64_t lease, std::size_t cursor, std::size_t limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    expire();
    auto it = held_.find(lease);
    if (it == held_.end()) return {Status::Expired, {}, 0, false};
    const auto& rows = it->second.snapshot.rows;
    if (!limit || limit > limits_.pageRows || cursor > rows.size())
        return {Status::Invalid, {}, 0, false};
    const auto end = cursor + std::min(limit, rows.size() - cursor);
    return {Status::Ok, {rows.begin() + cursor, rows.begin() + end}, end, end == rows.size()};
}
void CommittedEventStore::release(std::uint64_t lease) {
    std::lock_guard<std::mutex> lock(mutex_);
    expire();
    auto it = held_.find(lease);
    if (it != held_.end()) {
        heldBytes_ -= it->second.bytes;
        held_.erase(it);
    }
}
} // namespace backend::evidence
