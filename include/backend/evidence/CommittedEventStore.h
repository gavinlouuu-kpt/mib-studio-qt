#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace backend::evidence {
// Private E-event/1 model, not a bridge ABI or migration of F-contracts/1.
struct Identity {
    std::string source, session;
    std::optional<std::string> run; // monitoring outside an experiment has no run
    std::string epoch, revision;
    std::uint64_t generation{0};
    bool operator==(const Identity&) const;
};
struct Parameter {
    std::string id, unit;
};
struct Provenance {
    std::optional<std::string> fingerprint, processing, calibration, mechanics, clock;
    std::string sourceKind, eventMode, denominator;
    std::vector<Parameter> parameters;
};
// The injected owner must hold its source/revision read fence for this object's
// entire lifetime. acquire() returns null for stale identity. Never reenter store
// from the authority. Lock order is authority fence -> store mutex.
class Authority {
public:
    struct Lease {
        virtual ~Lease() = default;
    };
    virtual ~Authority() = default;
    virtual std::unique_ptr<Lease> acquire(const Identity& expected) = 0;
};
struct EventRow {
    std::string set;
    std::uint64_t ordinal{0};
    std::optional<std::uint64_t> frame, object, track;
    std::vector<std::optional<double>> values;
    std::string qc;
};
struct Accounting {
    std::uint64_t admittedFrames{0}, completedFrames{0}, errorFrames{0}, droppedFrames{0};
    std::uint64_t observedEvents{0}, missingEvents{0};
    bool operator==(const Accounting&) const;
};
struct Batch {
    std::uint64_t sequence{0};
    std::vector<EventRow> rows;
    Accounting accounting;
};
struct Coverage {
    std::uint64_t begin{0}, end{0};
}; // half-open batch sequence range
struct Gap {
    Coverage batches;
    std::optional<std::uint64_t> events; // null when lost frames have unknown object count
    std::string reason;
};
struct Snapshot {
    Identity identity;
    Provenance provenance;
    Coverage requested, observed;
    std::uint64_t watermark{0}; // exclusive next batch, including zero-row batches
    Accounting accounting;
    bool finalized{false}, requestedCovered{false};
    bool eventCountKnown{true}; // false after unobserved error/drop frames
    // Independent evidence properties, never inferred from metadata publication.
    bool durableAcknowledged{false}, imageRetentionGuaranteed{false};
    std::vector<EventRow> rows;
    std::vector<Gap> gaps;
};
enum class Status { Ok, Stale, Invalid, SequenceConflict, Finalized, ResourceExhausted, Expired };
struct LeaseResult {
    Status status{Status::Invalid};
    std::uint64_t lease{0};
};
struct Page {
    Status status{Status::Invalid};
    std::vector<EventRow> rows;
    std::size_t next{0};
    bool done{false};
};
struct Limits {
    std::size_t storeBytes{4 * 1024 * 1024}, leaseBytes{8 * 1024 * 1024}, leases{8}, pageRows{1024};
    std::chrono::milliseconds leaseLifetime{30000};
};
class CommittedEventStore {
public:
    using Clock = std::chrono::steady_clock;
    CommittedEventStore(Authority&, Limits = {},
                        std::function<Clock::time_point()> now = Clock::now);
    Status beginEpoch(const Identity&, const Provenance&);
    // Strict next sequence: ALL duplicates (including identical retransmits) and
    // skipped sequences are rejected without mutation. No unbounded dedup cache.
    Status commitBatch(const Identity&, const Batch&);
    Status finalize(const Identity&, const Accounting& exactTotals);
    // Snapshot always includes the entire committed prefix. requested must start
    // at zero; future requested end is allowed and is reported uncovered.
    LeaseResult acquireSnapshot(const Identity&, Coverage requested);
    std::optional<Snapshot> metadata(std::uint64_t lease); // rows excluded
    Page page(std::uint64_t lease, std::size_t cursor, std::size_t limit);
    void release(std::uint64_t lease);

private:
    struct Held {
        Snapshot snapshot;
        Clock::time_point expires;
        std::size_t bytes;
    };
    void expire();
    Authority& authority_;
    Limits limits_;
    std::function<Clock::time_point()> now_;
    std::mutex mutex_;
    std::optional<Snapshot> current_;
    std::size_t currentBytes_{0}, heldBytes_{0};
    std::uint64_t nextLease_{1}; // local handle only; NOT source identity authority
    std::map<std::uint64_t, Held> held_;
};
} // namespace backend::evidence
