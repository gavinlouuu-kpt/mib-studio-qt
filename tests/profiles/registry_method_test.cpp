// registry_method_test (#398 M2): the worker's local method jobs.
//  - every cached revision carries the canonical hash of its embedded
//    config.json, equal to canonicalConfigSha256 of the materialized file and
//    insensitive to key order / whitespace / integral doubles;
//  - Materialize writes config.json + camera script + canonical envelope,
//    read-only, idempotently; tampered files are replaced; unsafe revision IDs
//    and an unset methods dir are refused; the snapshot reports the dir, also
//    after a restart (verified scan);
//  - RecordValidation needs a signed-in user, a published/superseded revision,
//    and a readable evidence file; it records who, the instrument/context hash
//    and the evidence file's SHA-256; a revoked revision cannot be validated;
//  - the method context hash changes with the core build or camera source and
//    is empty for an unknown instrument.
#include "backend/profiles/ProfileRegistryWorker.h"
#include "backend/processing/ProcessingCoreSha256.h"

#include "support/assert.h"
#include "support/fake_supabase.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>

using namespace backend::profiles;
using Json = nlohmann::json;
using namespace std::chrono_literals;
using mib::test::FakeSupabase;
using mib::test::revisionJson;
using mib::test::transportFor;

namespace {

RegistryWorkerConfig config(const mib::test::TempDir& dir) {
    RegistryWorkerConfig c;
    c.origin = mib::test::kOrigin;
    c.publishableKey = mib::test::kKey;
    c.cacheDir = dir / "cache";
    c.methodsDir = dir / "methods";
    return c;
}

RegistryJobStatus run(ProfileRegistryWorker& worker, std::uint64_t id) {
    MIB_REQUIRE(id != 0, "request refused");
    MIB_REQUIRE(worker.waitForJob(id, 10s), "job did not finish");
    return worker.job(id);
}

const CachedRevisionSummary* find(const RegistryWorkerSnapshot& s, const std::string& id) {
    for (const auto& r : s.revisions)
        if (r.revisionId == id) return &r;
    return nullptr;
}

std::string readText(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

void writeText(const std::filesystem::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

MethodContext context(const std::string& instrument = "123e4567-e89b-42d3-a456-426614174000") {
    return {instrument, "1.2.3", std::string(64, 'c'), "mock"};
}

} // namespace

int main() {
    mib::test::Watchdog watchdog(60);
    mib::test::TempDir dir("mib_registry_method");

    watchdog.mark("canonical config hash");
    {
        const auto a = canonicalConfigSha256(R"({"b":2.0,"a":{"y":1,"x":"t"}})");
        const auto b = canonicalConfigSha256("{\n  \"a\": {\"x\": \"t\", \"y\": 1.0},\n  \"b\": 2\n}\n");
        MIB_EXPECT(!a.empty() && a == b, "key order, whitespace and integral doubles do not matter");
        MIB_EXPECT(a != canonicalConfigSha256(R"({"b":2.5,"a":{"y":1,"x":"t"}})"), "values matter");
        MIB_EXPECT(canonicalConfigSha256("[1,2]").empty(), "non-object config has no hash");
        MIB_EXPECT(canonicalConfigSha256("{bad").empty(), "invalid JSON has no hash");
        MIB_EXPECT(canonicalConfigSha256(R"({"a":1,"a":2})").empty(), "duplicate keys rejected");
        MIB_EXPECT(revisionConfigSha256("not an envelope").empty(), "unreadable envelope: no hash");
    }

    watchdog.mark("method context hash");
    {
        const auto base = methodContextHash(context());
        MIB_EXPECT(base.size() == 64 && base == methodContextHash(context()), "stable");
        auto other = context();
        other.processingCoreSha256 = std::string(64, 'd');
        MIB_EXPECT(methodContextHash(other) != base, "core build changes the context");
        other = context();
        other.cameraSource = "egrabber";
        MIB_EXPECT(methodContextHash(other) != base, "camera source changes the context");
        MIB_EXPECT(methodContextHash(context("223e4567-e89b-42d3-a456-426614174000")) != base,
                   "instrument changes the context");
        MIB_EXPECT(methodContextHash(context("")).empty(), "unknown instrument: no context");
    }

    FakeSupabase fake;
    fake.users["alice@lab"] = {"user-alice", "pw-alice", {"p1"}};
    fake.revisions["p1"] = {revisionJson("p1", "r1", 1, "published"),
                            revisionJson("p1", "r2", 2, "revoked"),
                            revisionJson("p1", "../escape", 3, "published")};
    const auto evidence = dir / "test-run.h5";
    writeText(evidence, std::string(300 * 1024, 'x'));

    watchdog.mark("materialize");
    {
        ProfileRegistryWorker worker(config(dir), transportFor(fake));
        MIB_REQUIRE(worker.waitIdle(5s), "initial load");
        MIB_EXPECT(run(worker, worker.requestMaterialize("r1")).state == RegistryJobState::Failed,
                   "no cache open: materialize fails");
        MIB_EXPECT(worker.requestMaterialize("../escape") == 0 && worker.requestMaterialize("") == 0 &&
                       worker.requestMaterialize(".hidden") == 0,
                   "unsafe revision IDs refused");
        MIB_REQUIRE(run(worker, worker.requestSignIn("alice@lab", "pw-alice")).state ==
                        RegistryJobState::Succeeded,
                    "sign in");
        MIB_REQUIRE(run(worker, worker.requestRefresh()).state == RegistryJobState::Succeeded,
                    "refresh");
        auto s = worker.snapshot();
        MIB_REQUIRE(find(s, "r1") != nullptr, "r1 cached");
        const CachedRevisionSummary r1Copy = *find(s, "r1"); // s is reassigned below
        const auto* r1 = &r1Copy;
        MIB_EXPECT(r1->configSha256 ==
                       canonicalConfigSha256(R"({"gain":1,"config_schema_version":1})"),
                   "snapshot carries the canonical config hash");
        MIB_EXPECT(r1->materializedDir.empty(), "not materialized yet");

        auto job = run(worker, worker.requestMaterialize("r1"));
        MIB_REQUIRE(job.state == RegistryJobState::Succeeded, job.message);
        MIB_EXPECT(job.kind == RegistryJobKind::Materialize, "job kind");
        const auto methodDir = dir / "methods" / "r1";
        MIB_EXPECT(canonicalConfigSha256(readText(methodDir / "config.json")) == r1->configSha256,
                   "materialized config.json matches the revision config hash");
        MIB_EXPECT(readText(methodDir / "egrabberConfig.js") == "camera();", "camera script written");
        MIB_EXPECT(contentHash(readText(methodDir / "method.canonical.json")) == r1->contentHash,
                   "canonical envelope written byte-exact");
        const auto perms = std::filesystem::status(methodDir / "config.json").permissions();
        MIB_EXPECT((perms & std::filesystem::perms::owner_write) == std::filesystem::perms::none,
                   "materialized files are read-only");
        MIB_EXPECT(!std::filesystem::exists(dir / "methods" / ".staging-r1"), "no staging left");
        s = worker.snapshot();
        MIB_EXPECT(find(s, "r1")->materializedDir == methodDir.string(), "snapshot reports the dir");

        job = run(worker, worker.requestMaterialize("r1"));
        MIB_EXPECT(job.state == RegistryJobState::Succeeded &&
                       job.message.find("Already") != std::string::npos,
                   "materialize is idempotent");

        // Tamper: the next materialize replaces the files.
        std::filesystem::permissions(methodDir / "config.json", std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::add);
        writeText(methodDir / "config.json", R"({"config_schema_version":1,"gain":999})");
        job = run(worker, worker.requestMaterialize("r1"));
        MIB_EXPECT(job.state == RegistryJobState::Succeeded &&
                       job.message.find("Materialized") != std::string::npos,
                   "tampered files rewritten");
        MIB_EXPECT(canonicalConfigSha256(readText(methodDir / "config.json")) == r1->configSha256,
                   "restored content");
        MIB_EXPECT(run(worker, worker.requestMaterialize("missing")).state ==
                       RegistryJobState::Failed,
                   "uncached revision fails");

        watchdog.mark("record validation");
        LocalValidationRequest request;
        request.revisionId = "r1";
        request.context = context();
        request.instrumentName = "MIB-01";
        request.evidenceFile = evidence.string();
        MIB_EXPECT(worker.requestRecordValidation({}) == 0, "empty request refused");
        auto noInstrument = request;
        noInstrument.context.instrumentId.clear();
        MIB_EXPECT(worker.requestRecordValidation(noInstrument) == 0, "unknown instrument refused");
        auto missingFile = request;
        missingFile.evidenceFile = (dir / "nope.h5").string();
        job = run(worker, worker.requestRecordValidation(missingFile));
        MIB_EXPECT(job.state == RegistryJobState::Failed &&
                       job.message.find("Evidence") != std::string::npos,
                   "unreadable evidence fails");
        auto revoked = request;
        revoked.revisionId = "r2";
        job = run(worker, worker.requestRecordValidation(revoked));
        MIB_EXPECT(job.state == RegistryJobState::Failed &&
                       job.message.find("revoked") != std::string::npos,
                   "a revoked revision cannot be validated");
        MIB_EXPECT(worker.snapshot().validations.empty(), "nothing recorded by refused requests");

        job = run(worker, worker.requestRecordValidation(request));
        MIB_REQUIRE(job.state == RegistryJobState::Succeeded, job.message);
        MIB_EXPECT(job.kind == RegistryJobKind::RecordValidation, "job kind");
        s = worker.snapshot();
        MIB_REQUIRE(s.validations.size() == 1, "validation listed");
        const auto& v = s.validations[0];
        MIB_EXPECT(v.validation.revisionId == "r1" && v.validation.passed &&
                       v.validation.validatorId == "user-alice" &&
                       v.validation.instrumentId == context().instrumentId &&
                       v.validation.contextHash == methodContextHash(context()) &&
                       v.validation.contentHash == r1->contentHash,
                   "who, instrument, context and content recorded");
        MIB_EXPECT(!v.validatedAtUtc.empty(), "when recorded");
        const auto ev = Json::parse(v.validation.evidence);
        std::string hashError;
        MIB_EXPECT(ev.at("run_file") == evidence.string() &&
                       ev.at("run_file_sha256") ==
                           backend::processing::fileSha256(evidence, &hashError, {}) &&
                       ev.at("run_file_bytes") == 300 * 1024 && ev.at("instrument_name") == "MIB-01" &&
                       ev.at("validator_email") == "alice@lab" &&
                       ev.at("context").at("camera_source") == "mock",
                   "evidence names the test-run file and its hash");

        auto failed = request;
        failed.passed = false;
        MIB_REQUIRE(run(worker, worker.requestRecordValidation(failed)).state ==
                        RegistryJobState::Succeeded,
                    "failed validation recorded");
        s = worker.snapshot();
        MIB_EXPECT(s.validations.size() == 1 && !s.validations[0].validation.passed,
                   "same instrument/context: latest outcome replaces the earlier one");
    }

    watchdog.mark("restart: offline cache keeps materialized dir + validations");
    {
        ProfileRegistryWorker worker(config(dir), transportFor(fake));
        MIB_REQUIRE(worker.waitIdle(5s), "initial load");
        auto s = worker.snapshot();
        MIB_EXPECT(s.session == RegistryWorkerSnapshot::Session::CachedOffline, "offline cache");
        MIB_EXPECT(find(s, "r1") && find(s, "r1")->materializedDir == (dir / "methods" / "r1").string(),
                   "verified materialized dir found by the scan");
        MIB_EXPECT(s.validations.size() == 1, "validations persist");
        LocalValidationRequest request;
        request.revisionId = "r1";
        request.context = context();
        request.evidenceFile = evidence.string();
        const auto job = run(worker, worker.requestRecordValidation(request));
        MIB_EXPECT(job.state == RegistryJobState::Failed &&
                       job.message.find("Sign in") != std::string::npos,
                   "offline cache: validation needs an authenticated user");
        MIB_EXPECT(run(worker, worker.requestMaterialize("r1")).state == RegistryJobState::Succeeded,
                   "materialize works from the offline cache");
    }

    watchdog.mark("no methods dir");
    {
        auto c = config(dir);
        c.methodsDir.clear();
        ProfileRegistryWorker worker(c, transportFor(fake));
        MIB_EXPECT(worker.requestMaterialize("r1") == 0, "materialize disabled without a methods dir");
    }

    watchdog.mark("cancel while hashing evidence");
    {
        // A large evidence file and an immediate cancel: Cancelled, not recorded.
        const auto big = dir / "big-run.h5";
        {
            std::ofstream out(big, std::ios::binary | std::ios::trunc);
            const std::string chunk(1 << 20, 'y');
            for (int i = 0; i < 64; ++i) out << chunk;
        }
        ProfileRegistryWorker worker(config(dir), transportFor(fake));
        MIB_REQUIRE(worker.waitIdle(5s), "initial load");
        MIB_REQUIRE(run(worker, worker.requestSignIn("alice@lab", "pw-alice")).state ==
                        RegistryJobState::Succeeded,
                    "sign in");
        const auto before = worker.snapshot().validations;
        LocalValidationRequest request;
        request.revisionId = "r1";
        request.context = context("323e4567-e89b-42d3-a456-426614174000");
        request.evidenceFile = big.string();
        const auto id = worker.requestRecordValidation(request);
        MIB_REQUIRE(id != 0, "queued");
        while (worker.job(id).state == RegistryJobState::Queued) std::this_thread::sleep_for(100us);
        worker.cancelAll();
        MIB_REQUIRE(worker.waitForJob(id, 10s), "finished");
        const auto state = worker.job(id).state;
        // Hashing 64 MiB normally takes far longer than the cancel round trip;
        // if the machine finished first the validation is simply recorded.
        if (state == RegistryJobState::Cancelled)
            MIB_EXPECT(worker.snapshot().validations.size() == before.size(),
                       "cancelled validation not recorded");
        else
            MIB_EXPECT(state == RegistryJobState::Succeeded, "otherwise it completed");
    }
    return mib::test::exitCode();
}
