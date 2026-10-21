#include "backend/app/CalibrationStore.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace backend::app {

namespace {

using nlohmann::json;
constexpr const char* kStoreKind = "mib.instrument_calibration";
constexpr uint32_t kStoreSchemaVersion = 1;
constexpr std::uintmax_t kStoreMaxFileBytes = 32u * 1024u * 1024u;

// A temp file, flushed to the device, renamed over the store: a failure leaves the old file.
bool writeAtomically(const std::filesystem::path& path, const std::string& text, std::string* why)
{
    const auto tmp = path.string() + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) {
        if (why) *why = "cannot create " + tmp;
        return false;
    }
    bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size() && std::fflush(f) == 0;
#ifdef _WIN32
    ok = ok && _commit(_fileno(f)) == 0;
#else
    ok = ok && ::fsync(fileno(f)) == 0;
#endif
    ok = std::fclose(f) == 0 && ok;
    std::error_code ec;
    if (ok) std::filesystem::rename(tmp, path, ec);
    if (!ok || ec) {
        std::filesystem::remove(tmp, ec);
        if (why) *why = "cannot write " + path.string();
        return false;
    }
    return true;
}

} // namespace

const char* toString(CalibrationApplyOutcome outcome)
{
    switch (outcome) {
    case CalibrationApplyOutcome::Applied: return "applied";
    case CalibrationApplyOutcome::NotFound: return "notFound";
    case CalibrationApplyOutcome::Insufficient: return "insufficient";
    case CalibrationApplyOutcome::NotApplicable: return "notApplicable";
    case CalibrationApplyOutcome::StaleFactor: return "staleFactor";
    case CalibrationApplyOutcome::RunActive: return "runActive";
    case CalibrationApplyOutcome::IdentityMismatch: return "identityMismatch";
    case CalibrationApplyOutcome::StoreFailed: return "storeFailed";
    case CalibrationApplyOutcome::InvalidRequest: return "invalidRequest";
    }
    return "unknown";
}

CalibrationStore::CalibrationStore(std::filesystem::path file) : file_(std::move(file)) { load(); }

void CalibrationStore::load()
{
    std::lock_guard<std::mutex> lk(mutex_);
    versions_.clear();
    error_.clear();
    std::error_code ec;
    if (!std::filesystem::exists(file_, ec)) {
        if (ec) error_ = "cannot check " + file_.string() + ": " + ec.message();
        return;
    }
    const auto size = std::filesystem::file_size(file_, ec);
    if (ec || size > kStoreMaxFileBytes) {
        error_ = ec ? "cannot read " + file_.string() : file_.string() + " is larger than the store allows";
        return;
    }
    std::ifstream in(file_, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    if (!in.good() && !in.eof()) {
        error_ = "cannot read " + file_.string();
        return;
    }
    try {
        const auto root = json::parse(buffer.str());
        if (!root.is_object() || root.value("kind", std::string{}) != kStoreKind ||
            root.value("schema_version", 0u) != kStoreSchemaVersion || !root.contains("versions") ||
            !root.at("versions").is_array())
            throw std::runtime_error("not a calibration store of schema 1");
        uint64_t lastSequence = 0;
        for (const auto& v : root.at("versions")) {
            CalibrationRecord rec;
            std::string why;
            if (!calibrationRecordFromJson(v.dump(), rec, &why)) throw std::runtime_error(why);
            if (rec.sequence <= lastSequence) throw std::runtime_error("versions are not in append order");
            lastSequence = rec.sequence;
            versions_.push_back(std::move(rec));
        }
        if (versions_.size() > kCalibrationStoreMaxVersions) throw std::runtime_error("more versions than the store allows");
    } catch (const std::exception& e) {
        versions_.clear();
        error_ = file_.string() + " is unreadable (" + e.what() + "); it is left untouched and no record is added";
        SPDLOG_ERROR("CalibrationStore: {}", error_);
    }
}

std::string CalibrationStore::error() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return error_;
}

uint64_t CalibrationStore::revision() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return versions_.size();
}

std::optional<CalibrationRecord> CalibrationStore::latestLocked(const std::string& recordId) const
{
    for (auto it = versions_.rbegin(); it != versions_.rend(); ++it)
        if (it->recordId == recordId) return *it;
    return std::nullopt;
}

std::optional<CalibrationRecord> CalibrationStore::appliedLocked() const
{
    // The newest Applied version whose record has no newer version.
    for (auto it = versions_.rbegin(); it != versions_.rend(); ++it) {
        if (it->status != CalibrationStatus::Applied) continue;
        const auto latest = latestLocked(it->recordId);
        if (latest && latest->sequence == it->sequence) return *it;
    }
    return std::nullopt;
}

std::vector<CalibrationRecord> CalibrationStore::latestRecords() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<CalibrationRecord> out;
    std::vector<std::string> seen;
    // Newest record first: order by the creation of each record (its first version).
    for (auto it = versions_.rbegin(); it != versions_.rend(); ++it) {
        if (std::find(seen.begin(), seen.end(), it->recordId) != seen.end()) continue;
        seen.push_back(it->recordId);
        out.push_back(*it);
    }
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.createdAtUnixMs != b.createdAtUnixMs ? a.createdAtUnixMs > b.createdAtUnixMs
                                                      : a.recordId > b.recordId;
    });
    return out;
}

std::optional<CalibrationRecord> CalibrationStore::latest(const std::string& recordId) const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return latestLocked(recordId);
}

std::vector<CalibrationRecord> CalibrationStore::history(const std::string& recordId) const
{
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<CalibrationRecord> out;
    for (const auto& v : versions_)
        if (v.recordId == recordId) out.push_back(v);
    return out;
}

std::optional<CalibrationRecord> CalibrationStore::applied() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return appliedLocked();
}

bool CalibrationStore::appendLocked(std::vector<CalibrationRecord> versions, std::string* why)
{
    if (!error_.empty()) {
        if (why) *why = error_;
        return false;
    }
    if (versions_.size() + versions.size() > kCalibrationStoreMaxVersions) {
        if (why)
            *why = "the calibration store holds " + std::to_string(versions_.size()) + " versions, its limit is " +
                   std::to_string(kCalibrationStoreMaxVersions);
        return false;
    }
    uint64_t sequence = versions_.empty() ? 0 : versions_.back().sequence;
    for (auto& v : versions) v.sequence = ++sequence;
    json all = json::array();
    for (const auto& v : versions_) all.push_back(json::parse(calibrationRecordToJson(v)));
    for (const auto& v : versions) all.push_back(json::parse(calibrationRecordToJson(v)));
    const json root = {{"kind", kStoreKind}, {"schema_version", kStoreSchemaVersion}, {"versions", all}};
    if (failWrites_) {
        if (why) *why = "cannot write " + file_.string() + " (fault injection)";
        return false;
    }
    std::error_code ec;
    if (file_.has_parent_path()) std::filesystem::create_directories(file_.parent_path(), ec);
    if (!writeAtomically(file_, root.dump(1) + "\n", why)) return false;
    for (auto& v : versions) versions_.push_back(std::move(v));
    return true;
}

bool CalibrationStore::addDerived(CalibrationRecord record, std::string* why)
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (record.recordId.empty() || record.recordId.size() > kCalibrationRecordIdMaxBytes) {
        if (why) *why = "invalid record id";
        return false;
    }
    if (latestLocked(record.recordId)) {
        if (why) *why = "record " + record.recordId + " already exists";
        return false;
    }
    if (record.status != CalibrationStatus::Proposed && record.status != CalibrationStatus::Insufficient) {
        if (why) *why = "a derived record is proposed or insufficient";
        return false;
    }
    record.version = 1;
    std::vector<CalibrationRecord> one;
    one.push_back(std::move(record));
    return appendLocked(std::move(one), why);
}

bool CalibrationStore::invalidateOnIdentityChange(const CalibrationIdentity& current, uint64_t nowUnixMs,
                                                  std::string* note)
{
    if (!current.known()) return false;
    std::lock_guard<std::mutex> lk(mutex_);
    const auto applied = appliedLocked();
    if (!applied || !applied->identity.known() || applied->identity == current) return false;
    CalibrationRecord next = *applied;
    next.version = applied->version + 1;
    next.status = CalibrationStatus::Invalidated;
    next.statusAtUnixMs = nowUnixMs;
    const auto change = describeIdentityChange(applied->identity, current);
    next.statusReason = "invalidated: " + change;
    next.appliedPreviousFactor.reset();
    next.appliedNewFactor.reset();
    std::string why;
    std::vector<CalibrationRecord> one;
    one.push_back(std::move(next));
    if (!appendLocked(std::move(one), &why)) {
        SPDLOG_ERROR("CalibrationStore: record {} could not be invalidated ({}): {}", applied->recordId, change, why);
        return false;
    }
    SPDLOG_WARN("CalibrationStore: applied record {} invalidated: {}", applied->recordId, change);
    if (note) *note = "record " + applied->recordId + " was invalidated: " + change;
    return true;
}

CalibrationApplyResult CalibrationStore::apply(const std::string& recordId, double expectedCurrentFactor,
                                               double currentFactor, const CalibrationIdentity& current,
                                               uint64_t nowUnixMs)
{
    CalibrationApplyResult r;
    r.recordId = recordId;
    if (recordId.empty() || recordId.size() > kCalibrationRecordIdMaxBytes) {
        r.outcome = CalibrationApplyOutcome::InvalidRequest;
        r.message = "a record id of 1 to " + std::to_string(kCalibrationRecordIdMaxBytes) + " bytes is required";
        return r;
    }
    if (!std::isfinite(expectedCurrentFactor) || expectedCurrentFactor <= 0.0) {
        r.outcome = CalibrationApplyOutcome::InvalidRequest;
        r.message = "expected_current_factor must be a finite positive number";
        return r;
    }
    std::lock_guard<std::mutex> lk(mutex_);
    if (!error_.empty()) {
        r.outcome = CalibrationApplyOutcome::StoreFailed;
        r.message = error_;
        return r;
    }
    const auto rec = latestLocked(recordId);
    if (!rec) {
        r.outcome = CalibrationApplyOutcome::NotFound;
        r.message = "no calibration record " + recordId;
        return r;
    }
    if (rec->status == CalibrationStatus::Insufficient) {
        r.outcome = CalibrationApplyOutcome::Insufficient;
        r.message = "record " + recordId + " is insufficient (it failed a minimum-evidence gate) and can never be applied";
        return r;
    }
    const bool reapply = rec->status == CalibrationStatus::Applied && rec->appliedNewFactor &&
                         !sameCalibrationFactor(*rec->appliedNewFactor, currentFactor);
    if (rec->status != CalibrationStatus::Proposed && !reapply) {
        r.outcome = CalibrationApplyOutcome::NotApplicable;
        r.message = rec->status == CalibrationStatus::Applied
                        ? "record " + recordId + " is already applied and in effect"
                        : "record " + recordId + " is " + toString(rec->status) + "; only a proposed record is applied";
        return r;
    }
    const auto proposed = reapply ? rec->appliedNewFactor : rec->proposedFactor();
    if (!proposed || !std::isfinite(*proposed) || *proposed <= 0.0) {
        r.outcome = CalibrationApplyOutcome::NotApplicable;
        r.message = "record " + recordId + " proposes no pixel-to-micron factor";
        return r;
    }
    if (!current.known()) {
        r.outcome = CalibrationApplyOutcome::IdentityMismatch;
        r.message = "the camera or ROI identity is unknown now; connect the camera and let the ROI apply first";
        return r;
    }
    if (rec->identity != current) {
        r.outcome = CalibrationApplyOutcome::IdentityMismatch;
        r.message = "record " + recordId + " was derived under another setup (" +
                    describeIdentityChange(rec->identity, current) + ")";
        return r;
    }
    if (!sameCalibrationFactor(expectedCurrentFactor, currentFactor)) {
        std::ostringstream m;
        m.precision(17);
        m << "the current pixel-to-micron factor is " << currentFactor << ", not the expected " << expectedCurrentFactor
          << "; nothing was applied";
        r.outcome = CalibrationApplyOutcome::StaleFactor;
        r.message = m.str();
        r.previousFactor = currentFactor;
        return r;
    }
    std::vector<CalibrationRecord> versions;
    const auto previous = appliedLocked();
    if (previous && previous->recordId != recordId) {
        CalibrationRecord sup = *previous;
        sup.version = previous->version + 1;
        sup.status = CalibrationStatus::Superseded;
        sup.statusAtUnixMs = nowUnixMs;
        sup.statusReason = "superseded by " + recordId;
        sup.supersededBy = recordId;
        sup.appliedPreviousFactor.reset();
        sup.appliedNewFactor.reset();
        versions.push_back(std::move(sup));
        r.supersededRecordId = previous->recordId;
    }
    CalibrationRecord next = *rec;
    next.version = rec->version + 1;
    next.status = CalibrationStatus::Applied;
    next.statusAtUnixMs = nowUnixMs;
    next.statusReason = reapply ? "re-applied by the controlling client" : "applied by the controlling client";
    next.appliedPreviousFactor = currentFactor;
    next.appliedNewFactor = *proposed;
    next.supersededBy.clear();
    versions.push_back(std::move(next));
    std::string why;
    if (!appendLocked(std::move(versions), &why)) {
        r.outcome = CalibrationApplyOutcome::StoreFailed;
        r.message = "the record could not be stored, nothing was applied: " + why;
        r.supersededRecordId.clear();
        return r;
    }
    r.outcome = CalibrationApplyOutcome::Applied;
    r.previousFactor = currentFactor;
    r.newFactor = *proposed;
    std::ostringstream m;
    m.precision(10);
    m << "record " << recordId << " applied: pixel-to-micron " << currentFactor << " -> " << *proposed;
    if (!r.supersededRecordId.empty()) m << " (supersedes " << r.supersededRecordId << ")";
    r.message = m.str();
    return r;
}

void CalibrationStore::setFailWritesForTesting(bool fail)
{
    std::lock_guard<std::mutex> lk(mutex_);
    failWrites_ = fail;
}

} // namespace backend::app
