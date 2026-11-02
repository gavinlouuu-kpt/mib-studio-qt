#include "backend/workspace/RecipeProvider.h"

#include "backend/processing/ProcessingCoreSha256.h"

#include <cstdint>
#include <regex>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace backend::workspace {
using J = nlohmann::json;
namespace {

constexpr const char* kNamespace = "recipe/";
constexpr const char* kType = "I-recipe/1";

struct Refusal : std::runtime_error {
    std::string status;
    Refusal(std::string s, const std::string& message)
        : std::runtime_error(message), status(std::move(s)) {}
};
[[noreturn]] void refuse(const char* status, const std::string& message) {
    throw Refusal(status, message);
}

std::string str(const J& v) { return v.get<std::string>(); }

std::string identity(const J& value, const std::string& what) {
    static const std::regex pattern("^[A-Za-z0-9][A-Za-z0-9._:-]{0,127}$");
    if (!value.is_string() || !std::regex_match(str(value), pattern))
        refuse("invalid", "Invalid identity: " + what);
    return str(value);
}
std::string fieldIdentity(const J& q, const char* key) {
    if (!q.contains(key)) refuse("invalid", std::string("Missing field: ") + key);
    return identity(q.at(key), std::string(key));
}
std::string text(const J& q, const char* key, std::size_t max, const std::string& fallback) {
    if (!q.contains(key)) return fallback;
    if (!q.at(key).is_string()) refuse("invalid", std::string("Expected text: ") + key);
    const auto v = str(q.at(key));
    if (v.size() > max) refuse("invalid", std::string("Text too long: ") + key);
    for (unsigned char c : v)
        if ((c < 32 && c != '\n' && c != '\t') || c == 127)
            refuse("invalid", std::string("Control characters are not allowed: ") + key);
    return v;
}
void fields(const J& q, const std::set<std::string>& required, const std::set<std::string>& optional) {
    if (!q.is_object()) refuse("invalid", "Expected a recipe request object");
    for (const auto& k : required)
        if (!q.contains(k)) refuse("invalid", "Missing field: " + k);
    for (auto it = q.begin(); it != q.end(); ++it)
        if (!required.count(it.key()) && !optional.count(it.key()))
            refuse("invalid", "Unknown recipe request field: " + it.key());
}
J revisionRef(const J& v, const std::string& what) {
    if (!v.is_object() || v.size() != 2 || !v.contains("id") || !v.contains("revision"))
        refuse("invalid", what + " requires exactly {id, revision}");
    return J{{"id", identity(v.at("id"), what + ".id")},
             {"revision", identity(v.at("revision"), what + ".revision")}};
}
J methodRef(const J& v) {
    if (!v.is_object() || v.size() != 3 || !v.contains("source"))
        refuse("invalid", "Method requires exactly {source, id, revision}");
    const auto source = v.at("source");
    if (source != "local" && source != "registry")
        refuse("invalid", "Method source must be local or registry");
    return J{{"source", source},
             {"id", identity(v.at("id"), std::string("method.id"))},
             {"revision", identity(v.at("revision"), std::string("method.revision"))}};
}
std::string sha(const std::string& bytes) {
    return processing::processingCoreBytesSha256(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                                                 bytes.size());
}
std::string shortHash(const std::string& h) { return h.size() > 12 ? h.substr(0, 12) : h; }
std::string describe(const J& m) {
    return str(m.at("source")) + " " + str(m.at("id")) + " @ " + str(m.at("revision"));
}

struct Stored {
    std::string revision;
    J document;
};

J item(const char* dimension, const char* status, const std::string& reason, J left, J right) {
    return J{{"dimension", dimension}, {"status", status}, {"reason", reason},
             {"left", std::move(left)}, {"right", std::move(right)}};
}
J opt(const std::optional<std::string>& v) { return v ? J(*v) : J(nullptr); }
J opt(const std::optional<int>& v) { return v ? J(*v) : J(nullptr); }

// The calibration policy the method's config.json declares; nullopt with `why` when unreadable.
std::optional<std::string> policyOf(const RecipeMethodFacts& f, std::string& why) {
    if (!f.resolved) { why = f.reason; return std::nullopt; }
    if (!f.calibrationPolicy) why = f.calibrationReason;
    return f.calibrationPolicy;
}

struct Calibration {
    bool known{false};
    std::string reason;
    J recordId;      // string or null (none in effect)
    bool lutLoaded{false};
    std::string lutSha256;
};
Calibration basisOf(const J& doc) {
    Calibration c;
    const auto& b = doc.at("basis");
    c.known = b.at("calibration_known").get<bool>();
    if (!c.known) {
        c.reason = b.value("reason", std::string{"no calibration basis was recorded"});
        return c;
    }
    c.recordId = b.at("calibration_record_id");
    c.lutLoaded = b.at("emodulus_lut").at("loaded").get<bool>();
    const auto& s = b.at("emodulus_lut").at("sha256");
    c.lutSha256 = s.is_string() ? str(s) : std::string{};
    return c;
}
Calibration calibrationOf(const RecipeInstrumentState& s) {
    Calibration c;
    c.known = s.calibrationKnown;
    c.reason = s.calibrationReason;
    c.recordId = opt(s.calibrationRecordId);
    c.lutLoaded = s.lutLoaded;
    c.lutSha256 = s.lutSha256;
    return c;
}
J calibrationJson(const Calibration& c) {
    if (!c.known) return J{{"known", false}, {"reason", c.reason}};
    return J{{"known", true}, {"calibration_record_id", c.recordId},
             {"emodulus_lut", {{"loaded", c.lutLoaded},
                               {"sha256", c.lutSha256.empty() ? J(nullptr) : J(c.lutSha256)}}}};
}

// Accumulates per-field outcomes of one dimension: any difference wins, then any unknown.
struct Verdict {
    std::vector<std::string> different, unknown, equal;
    J result(const char* dimension, J left, J right) const {
        auto join = [](const std::vector<std::string>& v) {
            std::string out;
            for (const auto& s : v) out += (out.empty() ? "" : "; ") + s;
            return out;
        };
        if (!different.empty()) return item(dimension, "different", join(different), left, right);
        if (!unknown.empty()) return item(dimension, "unknown", join(unknown), left, right);
        return item(dimension, "equal", join(equal), left, right);
    }
};

struct Side {
    std::string label;                    // "recipe A @ 1", "instrument"
    J method;                             // {source,id,revision}; null for the instrument
    RecipeMethodFacts facts;
    Calibration calibration;
    std::optional<RecipeInstrumentState> instrument;
    std::string storedContent;  // the content_sha256 the recipe recorded; empty: not recorded
    bool contentChanged{false}; // the method authority's content for the revision differs from it
};

J contentOf(const Side& s) { return s.storedContent.empty() ? J(nullptr) : J(s.storedContent); }

// The method revision identity, then its content: the same {source, id, revision} with another
// content hash is a different method ("method content changed for this revision").
constexpr const char* kContentChanged = "method content changed for this revision";

J methodItem(const Side& l, const Side& r) {
    const auto& m = l.method;
    J left = m;
    left["content_sha256"] = contentOf(l);
    if (!r.instrument) {
        J right = r.method;
        right["content_sha256"] = contentOf(r);
        if (m != r.method)
            return item("method", "different",
                        l.label + " pins " + describe(m) + "; " + r.label + " pins " +
                            describe(r.method),
                        left, right);
        for (const Side* s : {&l, &r})
            if (s->storedContent.empty())
                return item("method", "unknown",
                            "Both pin " + describe(m) + ", but " + s->label +
                                " recorded no method content hash",
                            left, right);
        if (l.storedContent != r.storedContent)
            return item("method", "different",
                        std::string(kContentChanged) + ": both pin " + describe(m) + ", " + l.label +
                            " recorded content " + shortHash(l.storedContent) + " and " + r.label +
                            " " + shortHash(r.storedContent),
                        left, right);
        return item("method", "equal",
                    "Both pin " + describe(m) + " (content " + shortHash(l.storedContent) + ")", left,
                    right);
    }
    const auto& s = *r.instrument;
    J right = {{"source", s.methodSource}, {"id", s.methodId}, {"revision", s.methodRevision},
               {"named", s.methodNamed}, {"modified", s.methodModified}};
    if (s.methodSource == "none")
        return item("method", "different", "No method is applied on the instrument", left, right);
    if (!s.methodNamed)
        return item("method", "unknown",
                    "The applied configuration does not name a method revision (it was applied "
                    "outside the registry and the local profile store)",
                    left, right);
    const auto source = s.methodSource == "central" ? "registry" : "local";
    if (s.methodModified)
        return item("method", "different",
                    "The applied configuration was changed after local profile " + s.methodId +
                        " was applied",
                    left, right);
    const bool same = m.at("source") == source && m.at("id") == s.methodId &&
                      m.at("revision") == s.methodRevision;
    if (!same)
        return item("method", "different",
                    "Recipe pins " + describe(m) + "; the instrument runs " + source + " " +
                        s.methodId + " @ " + s.methodRevision,
                    left, right);
    // The instrument runs the revision as the method authority holds it now.
    right["content_sha256"] = (l.facts.resolved || l.contentChanged) && !l.facts.contentSha256.empty()
                                  ? J(l.facts.contentSha256)
                                  : J(nullptr);
    if (l.contentChanged)
        return item("method", "different",
                    std::string(kContentChanged) + ": the instrument runs " + describe(m) +
                        ", whose content is no longer the " + shortHash(l.storedContent) + " " +
                        l.label + " recorded",
                    left, right);
    if (l.storedContent.empty())
        return item("method", "unknown",
                    "The instrument runs " + describe(m) + ", but " + l.label +
                        " recorded no method content hash",
                    left, right);
    if (right.at("content_sha256").is_null())
        return item("method", "unknown",
                    "The instrument runs " + describe(m) +
                        ", but the method authority gives no content hash for it: " +
                        (l.facts.resolved ? std::string("none exposed") : l.facts.reason),
                    left, right);
    return item("method", "equal",
                "The instrument runs " + describe(m) + " (content " + shortHash(l.storedContent) + ")",
                left, right);
}

J profileItem(const Side& l, const Side& r) {
    auto sideSha = [](const Side& s, std::string& why) -> std::string {
        if (s.instrument) {
            if (s.instrument->configSha256.empty()) why = "no configuration is applied";
            return s.instrument->configSha256;
        }
        if (!s.facts.resolved) {
            why = "its method is not resolved: " + s.facts.reason;
            return {};
        }
        if (s.facts.configSha256.empty()) why = "the method authority exposes no config.json hash";
        return s.facts.configSha256;
    };
    std::string lw, rw;
    const auto ls = sideSha(l, lw), rs = sideSha(r, rw);
    const J left = ls.empty() ? J(nullptr) : J(ls), right = rs.empty() ? J(nullptr) : J(rs);
    if (r.instrument && rs.empty())
        return item("profile", "different", "No configuration is applied on the instrument", left,
                    right);
    if (ls.empty())
        return item("profile", "unknown", "Processing profile of " + l.label + " unknown: " + lw,
                    left, right);
    if (rs.empty())
        return item("profile", "unknown", "Processing profile of " + r.label + " unknown: " + rw,
                    left, right);
    return item("profile", ls == rs ? "equal" : "different",
                ls == rs ? "Same canonical config.json (" + shortHash(ls) + ")"
                         : "Canonical config.json differs (" + shortHash(ls) + " vs " +
                               shortHash(rs) + ")",
                left, right);
}

J coreItem(const Side& l, const Side& r) {
    struct Core {
        bool known{false};
        std::string why;
        std::optional<std::string> version;
        std::optional<int> contract;
        bool required{true}; // a method declaration is a requirement; the instrument is a fact
    };
    auto coreOf = [](const Side& s) {
        Core c;
        if (s.instrument) {
            c.known = true;
            c.required = false;
            c.version = s.instrument->coreVersion;
            c.contract = s.instrument->contractVersion;
            return c;
        }
        if (!s.facts.resolved) {
            c.why = "its method is not resolved: " + s.facts.reason;
            return c;
        }
        c.version = s.facts.coreVersion;
        c.contract = s.facts.contractVersion;
        c.known = c.version || c.contract;
        c.why = s.facts.coreReason.empty() ? "the method declares no processing core or contract"
                                           : s.facts.coreReason;
        return c;
    };
    const auto lc = coreOf(l), rc = coreOf(r);
    const J left = {{"core_version", opt(lc.version)}, {"contract_version", opt(lc.contract)}};
    const J right = {{"core_version", opt(rc.version)}, {"contract_version", opt(rc.contract)}};
    if (!lc.known)
        return item("core", "unknown", "Processing core of " + l.label + " unknown: " + lc.why,
                    left, right);
    if (!rc.known)
        return item("core", "unknown", "Processing core of " + r.label + " unknown: " + rc.why,
                    left, right);
    Verdict v;
    auto field = [&](const char* name, const auto& a, const auto& b, auto show) {
        if (a && b) {
            if (*a == *b) v.equal.push_back(std::string(name) + " " + show(*a));
            else
                v.different.push_back(l.label + " " + name + " " + show(*a) + "; " + r.label +
                                      " " + name + " " + show(*b));
        } else if (a || b) {
            // A side that runs (the instrument) satisfies any declaration; two method
            // declarations that pin different fields cannot be compared.
            if (!rc.required) v.equal.push_back(std::string("the method pins no ") + name);
            else v.unknown.push_back(std::string("only one recipe's method pins its ") + name);
        }
    };
    field("core", lc.version, rc.version, [](const std::string& s) { return s; });
    field("contract", lc.contract, rc.contract, [](int c) { return std::to_string(c); });
    return v.result("core", left, right);
}

J calibrationItem(const Side& l, const Side& r) {
    const auto& lc = l.calibration;
    const auto& rc = r.calibration;
    J left = calibrationJson(lc), right = calibrationJson(rc);
    std::string lwhy, rwhy;
    const auto lp = policyOf(l.facts, lwhy);
    const auto rp = r.instrument ? std::optional<std::string>{} : policyOf(r.facts, rwhy);
    left["method_policy"] = opt(lp);
    if (!r.instrument) right["method_policy"] = opt(rp);
    if (!lc.known)
        return item("calibration", "unknown",
                    "Calibration basis of " + l.label + " unknown: " + lc.reason, left, right);
    if (!rc.known)
        return item("calibration", "unknown",
                    "Calibration of " + r.label + " unknown: " + rc.reason, left, right);
    Verdict v;
    auto record = [](const J& id) { return id.is_string() ? "record " + str(id) : std::string("no record in effect"); };
    if (lc.recordId == rc.recordId) v.equal.push_back("calibration " + record(lc.recordId));
    else
        v.different.push_back(l.label + " was established under " + record(lc.recordId) + "; " +
                              r.label + " has " + record(rc.recordId));
    if (lc.lutLoaded != rc.lutLoaded)
        v.different.push_back(std::string("E-LUT ") + (lc.lutLoaded ? "loaded" : "not loaded") +
                              " for " + l.label + ", " + (rc.lutLoaded ? "loaded" : "not loaded") +
                              " for " + r.label);
    else if (!lc.lutLoaded) v.equal.push_back("no E-LUT loaded on either side");
    else if (lc.lutSha256.empty() || rc.lutSha256.empty())
        v.unknown.push_back("an E-LUT is loaded but its bytes were not identified");
    else if (lc.lutSha256 == rc.lutSha256) v.equal.push_back("E-LUT " + shortHash(lc.lutSha256));
    else
        v.different.push_back("E-LUT differs (" + shortHash(lc.lutSha256) + " vs " +
                              shortHash(rc.lutSha256) + ")");
    if (r.instrument) {
        if (!lp)
            v.unknown.push_back("the recipe method's calibration policy is not readable: " + lwhy);
        else if (*lp == "required" && !rc.recordId.is_string())
            v.different.push_back("the recipe's method requires an applied calibration record and "
                                  "none is in effect");
    } else if (lp && rp && *lp != *rp) {
        v.different.push_back("calibration policy " + *lp + " vs " + *rp);
    } else if (lp && rp) {
        v.equal.push_back("calibration policy " + *lp);
    } else {
        v.unknown.push_back("calibration policy not readable: " + (lp ? rwhy : lwhy));
    }
    return v.result("calibration", left, right);
}

std::string overall(const J& items) {
    bool unknown = false;
    for (const auto& i : items) {
        if (i.at("status") == "different") return "different";
        if (i.at("status") == "unknown") unknown = true;
    }
    return unknown ? "unknown" : "equal";
}

} // namespace

RecipeProvider::RecipeProvider(RecipeHost host) : host_(std::move(host)) {}

std::string RecipeProvider::request(const std::string& requestJson, bool write) const {
    try {
        if (requestJson.size() > 65536)
            return R"({"status":"invalid","message":"Recipe request exceeds 64 KiB"})";
        return handle(J::parse(requestJson), write, false).dump();
    } catch (const J::exception&) {
        return R"({"status":"invalid","message":"Malformed recipe request"})";
    }
}

J RecipeProvider::handle(const J& q, bool write, bool internal) const {
    try {
        if (!q.is_object() || !q.contains("action") || !q.at("action").is_string())
            refuse("invalid", "Expected a recipe request with an action");
        const auto action = str(q.at("action"));

        auto storeRead = [&](const std::string& id,
                             const std::optional<std::string>& revision) -> std::optional<Stored> {
            J request = {{"action", "read"}, {"workspace_id", kNamespace + id}};
            if (revision) request["revision"] = *revision;
            const auto reply = host_.store(request, false);
            const auto status = reply.value("status", std::string{"storage"});
            if (status == "not_found") return std::nullopt;
            if (status != "ok")
                refuse(status == "invalid" ? "invalid" : "storage",
                       reply.value("message", std::string{"Recipe store read failed"}));
            Stored s{str(reply.at("revision")), reply.at("document")};
            if (s.document.value("type", std::string{}) != kType)
                refuse("storage", "Stored recipe has an unsupported type");
            return s;
        };
        auto recipeJson = [](const Stored& s) {
            J r = s.document;
            r["revision"] = s.revision;
            return r;
        };
        auto readExact = [&](const J& ref, const std::string& what) {
            const auto r = revisionRef(ref, what);
            const auto s = storeRead(str(r.at("id")), str(r.at("revision")));
            if (!s) refuse("not_found", what + " " + str(r.at("id")) + " @ " +
                                            str(r.at("revision")) + " not found");
            return *s;
        };
        // F's analysis authority on a finding reference {id, revision}: "ok", or the refusal
        // status ("not_found" for a finding it does not know, "unavailable") with `why`.
        auto verifyFinding = [&](const J& ref, std::string& why) -> std::string {
            if (!host_.finding) {
                why = "no analysis authority is available to verify findings";
                return "unavailable";
            }
            const auto reply = host_.finding(str(ref.at("id")), str(ref.at("revision")));
            const auto status = reply.value("status", std::string{"unavailable"});
            if (status != "ok") {
                why = reply.value("message", status);
                return status == "not_found" || status == "invalid" ? "not_found" : "unavailable";
            }
            if (reply.value("findingId", J(nullptr)) != ref.at("id") ||
                reply.value("revision", J(nullptr)) != ref.at("revision") ||
                !reply.contains("document") || !reply.at("document").is_object() ||
                reply.at("document").value("type", std::string{}) != "F-analysis-finding/1") {
                why = "the analysis authority returned a different record";
                return "not_found";
            }
            return "ok";
        };
        auto validatePair = [&](const J& method, const J& plan, const std::string& base) {
            auto facts = host_.method(method, base);
            if (!facts.resolved)
                refuse("unavailable",
                       "Pinned acquisition method is not available from the method authority: " +
                           facts.reason);
            const auto p = host_.plan(str(plan.at("id")), str(plan.at("revision")));
            const auto status = p.value("status", std::string{"unavailable"});
            if (status != "ok")
                refuse(status == "not_found" ? "not_found" : "unavailable",
                       "Pinned analysis plan revision is not available from the analysis "
                       "authority: " + p.value("message", status));
            if (p.contains("document") && p.at("document").is_object() &&
                p.at("document").contains("method") && p.at("document").at("method").is_object()) {
                const auto& bound = p.at("document").at("method");
                if (bound.value("method_id", J(nullptr)) != method.at("id") ||
                    bound.value("revision_id", J(nullptr)) != method.at("revision") ||
                    (bound.contains("content_hash") && bound.at("content_hash") != facts.contentSha256))
                    refuse("invalid", "The analysis plan binds a different acquisition method");
            }
            return facts;
        };
        auto basis = [&]() {
            const auto s = host_.instrument();
            if (!s.calibrationKnown)
                return J{{"calibration_known", false}, {"reason", s.calibrationReason},
                         {"captured_at", host_.now()}};
            return J{{"calibration_known", true},
                     {"calibration_record_id", opt(s.calibrationRecordId)},
                     {"emodulus_lut",
                      {{"loaded", s.lutLoaded},
                       {"sha256", s.lutLoaded && !s.lutSha256.empty() ? J(s.lutSha256) : J(nullptr)}}},
                     {"readiness_generation", s.generation},
                     {"captured_at", host_.now()}};
        };
        // Create-only write with an exact replay: the same command and request return the stored
        // recipe; any other request for an existing identity is a conflict (recipes are immutable).
        // `build` validates and returns the document; `beforeCommit` runs only once it has passed
        // (an adopted parent is written there, so a refused derive writes nothing).
        std::function<J(const std::string&, const std::string&, const std::string&,
                        const std::function<J()>&, const std::function<void()>&)>
            writeRecipe;
        writeRecipe = [&](const std::string& id, const std::string& command,
                          const std::string& requestSha, const std::function<J()>& build,
                          const std::function<void()>& beforeCommit) -> J {
            auto replay = [&]() -> std::optional<J> {
                const auto existing = storeRead(id, std::nullopt);
                if (!existing) return std::nullopt;
                const auto& origin = existing->document.at("origin");
                if (origin.at("command_id") == command && origin.at("request_sha256") == requestSha)
                    return recipeJson(*existing);
                refuse("conflict", "Recipe " + id +
                                       " already exists; recipes are immutable, derive a new one");
            };
            if (auto prior = replay()) return J{{"status", "ok"}, {"replayed", true}, {"recipe", *prior}};
            J doc = build();
            doc["origin"] = {{"command_id", command}, {"request_sha256", requestSha}};
            if (beforeCommit) beforeCommit();
            const auto reply = host_.store(J{{"action", "commit"},
                                             {"workspace_id", kNamespace + id},
                                             {"expected_revision", nullptr},
                                             {"command_id", command},
                                             {"document", doc}},
                                           true);
            const auto status = reply.value("status", std::string{"storage"});
            if (status == "conflict")
                if (auto prior = replay()) return J{{"status", "ok"}, {"replayed", true}, {"recipe", *prior}};
            if (status != "ok")
                refuse(status == "conflict" || status == "invalid" ? status.c_str() : "storage",
                       reply.value("message", std::string{"Recipe store write failed"}));
            return J{{"status", "ok"}, {"replayed", false},
                     {"recipe", recipeJson(Stored{str(reply.at("revision")), reply.at("document")})}};
        };
        auto sideOf = [&](const Stored& s, const std::string& base) {
            Side side;
            side.label = "recipe " + str(s.document.at("id")) + " @ " + s.revision;
            const auto& m = s.document.at("method");
            side.method = J{{"source", m.at("source")}, {"id", m.at("id")}, {"revision", m.at("revision")}};
            side.facts = host_.method(side.method, base);
            if (m.contains("content_sha256") && m.at("content_sha256").is_string())
                side.storedContent = str(m.at("content_sha256"));
            if (side.facts.resolved && !side.storedContent.empty() &&
                side.facts.contentSha256 != side.storedContent) {
                side.contentChanged = true;
                side.facts.resolved = false;
                side.facts.reason = "the method authority's content for this revision differs "
                                    "from the hash recorded when the recipe was created";
            }
            side.calibration = basisOf(s.document);
            return side;
        };
        auto instrumentSide = [&]() {
            Side side;
            side.label = "the instrument";
            side.instrument = host_.instrument();
            side.calibration = calibrationOf(*side.instrument);
            return side;
        };
        auto diff = [&](const Side& l, const Side& r) {
            J items = J::array({methodItem(l, r), profileItem(l, r), coreItem(l, r),
                                calibrationItem(l, r)});
            return items;
        };

        if (action == "recipe_list") {
            fields(q, {"action"}, {"after", "limit"});
            int limit = 50;
            if (q.contains("limit")) {
                if (!q.at("limit").is_number_integer()) refuse("invalid", "limit must be 1..100");
                limit = q.at("limit").get<int>();
                if (limit < 1 || limit > 100) refuse("invalid", "limit must be 1..100");
            }
            const auto after = q.contains("after") ? fieldIdentity(q, "after") : std::string{};
            const auto reply = host_.store(J{{"action", "catalog"},
                                             {"workspace_id", kNamespace},
                                             {"after", after.empty() ? "" : kNamespace + after},
                                             {"limit", limit + 1}},
                                           false);
            if (reply.value("status", std::string{}) != "ok")
                refuse("storage", reply.value("message", std::string{"Recipe catalog failed"}));
            J rows = J::array();
            const auto& entries = reply.at("entries");
            for (std::size_t i = 0; i < entries.size() && static_cast<int>(i) < limit; ++i) {
                const auto id = str(entries.at(i).at("workspace_id")).substr(std::string(kNamespace).size());
                const auto s = storeRead(id, std::nullopt);
                if (!s) continue;
                const auto& d = s->document;
                rows.push_back({{"id", d.at("id")}, {"revision", s->revision}, {"title", d.at("title")},
                                {"created_at", d.at("created_at")}, {"method", d.at("method")},
                                {"analysis_plan", d.at("analysis_plan")}, {"lineage", d.at("lineage")}});
            }
            const bool more = static_cast<int>(entries.size()) > limit;
            return J{{"status", "ok"}, {"recipes", rows},
                     {"next_after", more && !rows.empty() ? rows.back().at("id") : J(nullptr)}};
        }
        if (action == "recipe_get") {
            fields(q, {"action", "recipe_id", "revision"}, {});
            const auto s = readExact(J{{"id", q.at("recipe_id")}, {"revision", q.at("revision")}}, "Recipe");
            return J{{"status", "ok"}, {"recipe", recipeJson(s)}};
        }
        if (action == "recipe_create") {
            if (!write) refuse("invalid", "Creating a recipe requires the control-only write endpoint");
            fields(q, {"action", "command_id", "recipe_id", "title", "method", "analysis_plan"},
                   {"note", "profile_base"});
            const auto id = fieldIdentity(q, "recipe_id");
            const auto command = fieldIdentity(q, "command_id");
            const auto title = text(q, "title", 200, "");
            if (title.empty()) refuse("invalid", "A recipe needs a title");
            const auto note = text(q, "note", 2000, "");
            const auto method = methodRef(q.at("method"));
            const auto plan = revisionRef(q.at("analysis_plan"), "analysis_plan");
            const auto base = text(q, "profile_base", 4096, "");
            return writeRecipe(id, command, sha(q.dump()), [&] {
                const auto facts = validatePair(method, plan, base);
                J m = method;
                m["content_sha256"] = facts.contentSha256;
                return J{{"schema_version", 1}, {"type", kType}, {"id", id}, {"title", title},
                         {"note", note}, {"created_at", host_.now()}, {"method", m},
                         {"analysis_plan", plan},
                         {"lineage", {{"recipe", nullptr}, {"finding", nullptr}}},
                         {"basis", basis()}};
            }, {});
        }
        if (action == "recipe_derive") {
            if (!write) refuse("invalid", "Deriving a recipe requires the control-only write endpoint");
            std::set<std::string> optional{"title", "note", "method", "analysis_plan", "finding",
                                           "profile_base"};
            if (internal) optional.insert("parent_document");
            fields(q, {"action", "command_id", "recipe_id", "parent"}, optional);
            const auto id = fieldIdentity(q, "recipe_id");
            const auto command = fieldIdentity(q, "command_id");
            const auto parentRef = revisionRef(q.at("parent"), "parent");
            if (str(parentRef.at("id")) == id)
                refuse("invalid", "A derived recipe needs a fresh identity");
            const auto base = text(q, "profile_base", 4096, "");
            const auto finding = q.contains("finding") ? revisionRef(q.at("finding"), "finding") : J(nullptr);
            // A finding is a reference into F's analysis authority, not just a well-formed pair.
            if (!finding.is_null()) {
                std::string why;
                const auto known = verifyFinding(finding, why);
                if (known != "ok")
                    refuse(known.c_str(), "Finding " + str(finding.at("id")) + " @ " +
                                              str(finding.at("revision")) +
                                              " is not known to the analysis authority: " + why);
            }
            // In-process planning: adopt the exact inline recipe of a saved workspace revision.
            // Workspace documents are user-written, so none of it becomes provider truth
            // unchecked: the pair is validated as recipe_create does, a lineage parent must already
            // be a provider recipe (never the recipe itself or the one being derived), and a
            // lineage finding must be known to the analysis authority. A reference that is not
            // adopted is dropped with its reason in adoption_notes. The adopted parent is written
            // only after the child has validated too, so a refused derive writes nothing.
            J parentDoc;
            std::optional<J> adopted;
            std::string adoptSha;
            if (internal && q.contains("parent_document") &&
                !storeRead(str(parentRef.at("id")), std::nullopt)) {
                const auto& p = q.at("parent_document");
                const auto pm = methodRef(p.at("method"));
                const auto pp = revisionRef(p.at("analysis_plan"), "analysis_plan");
                if (identity(p.at("id"), std::string("parent.id")) != str(parentRef.at("id")) ||
                    identity(p.at("revision"), std::string("parent.revision")) != str(parentRef.at("revision")))
                    refuse("invalid", "Parent document does not match the parent reference");
                if (str(parentRef.at("revision")) != "1")
                    refuse("invalid", "Only revision 1 workspace recipes can be adopted");
                J lineage = {{"recipe", nullptr}, {"finding", nullptr}};
                J notes = J::array();
                const bool hasLineage = p.contains("lineage") && p.at("lineage").is_object();
                if (hasLineage && p.at("lineage").contains("recipe") && p.at("lineage").at("recipe").is_object()) {
                    const auto ref = revisionRef(p.at("lineage").at("recipe"), "lineage.recipe");
                    const auto rid = str(ref.at("id"));
                    if (rid == str(parentRef.at("id")))
                        refuse("invalid", "Workspace recipe " + rid +
                                              " names itself as its parent; its lineage cannot be adopted");
                    if (rid == id)
                        refuse("invalid", "Workspace recipe " + str(parentRef.at("id")) +
                                              " names the recipe being derived (" + id +
                                              ") as its parent; its lineage cannot be adopted");
                    if (storeRead(rid, str(ref.at("revision")))) lineage["recipe"] = ref;
                    else
                        notes.push_back("lineage not adopted: parent not in provider (" + rid + " @ " +
                                        str(ref.at("revision")) + ")");
                }
                if (hasLineage && p.at("lineage").contains("finding") && p.at("lineage").at("finding").is_object()) {
                    const auto ref = revisionRef(p.at("lineage").at("finding"), "lineage.finding");
                    std::string why;
                    if (verifyFinding(ref, why) == "ok") lineage["finding"] = ref;
                    else
                        notes.push_back("lineage not adopted: finding " + str(ref.at("id")) + " @ " +
                                        str(ref.at("revision")) +
                                        " is not known to the analysis authority (" + why + ")");
                }
                const auto facts = validatePair(pm, pp, base);
                J m = pm;
                m["content_sha256"] = facts.contentSha256;
                adopted = J{{"schema_version", 1}, {"type", kType}, {"id", parentRef.at("id")},
                            {"title", text(p, "title", 200, "Workspace recipe")},
                            {"note", text(p, "question", 2000, "")},
                            {"created_at", host_.now()}, {"method", m}, {"analysis_plan", pp},
                            {"lineage", lineage}, {"adoption_notes", notes},
                            {"basis", {{"calibration_known", false},
                                       {"reason", "adopted from a saved workspace revision; "
                                                  "the calibration in effect when it was "
                                                  "authored was not recorded"},
                                       {"captured_at", host_.now()}}}};
                adoptSha = sha(J{{"adopt", p}}.dump());
                parentDoc = *adopted;
            } else {
                const auto parent = readExact(parentRef, "Parent recipe");
                if (internal && q.contains("parent_document")) {
                    const auto& p = q.at("parent_document");
                    const auto& stored = parent.document;
                    if (methodRef(p.at("method")) != J({{"source", stored.at("method").at("source")},
                                                        {"id", stored.at("method").at("id")},
                                                        {"revision", stored.at("method").at("revision")}}) ||
                        revisionRef(p.at("analysis_plan"), "analysis_plan") != stored.at("analysis_plan"))
                        refuse("conflict", "The workspace recipe differs from the provider's record of "
                                           "the same identity");
                }
                parentDoc = parent.document;
            }
            const auto& pd = parentDoc;
            const J method = q.contains("method") ? methodRef(q.at("method"))
                                                  : J{{"source", pd.at("method").at("source")},
                                                      {"id", pd.at("method").at("id")},
                                                      {"revision", pd.at("method").at("revision")}};
            const J plan = q.contains("analysis_plan") ? revisionRef(q.at("analysis_plan"), "analysis_plan")
                                                       : pd.at("analysis_plan");
            const auto title = text(q, "title", 200, str(pd.at("title")));
            const auto note = text(q, "note", 2000, "");
            J request = q;
            request.erase("parent_document");
            return writeRecipe(
                id, command, sha(request.dump()),
                [&] {
                    const auto facts = validatePair(method, plan, base);
                    J m = method;
                    m["content_sha256"] = facts.contentSha256;
                    return J{{"schema_version", 1}, {"type", kType}, {"id", id},
                             {"title", title.empty() ? str(pd.at("title")) : title}, {"note", note},
                             {"created_at", host_.now()}, {"method", m}, {"analysis_plan", plan},
                             {"lineage", {{"recipe", parentRef}, {"finding", finding}}},
                             {"basis", basis()}};
                },
                [&] {
                    if (adopted)
                        writeRecipe(str(parentRef.at("id")), "adopt-" + command, adoptSha,
                                    [&] { return *adopted; }, {});
                });
        }
        if (action == "recipe_diff") {
            fields(q, {"action", "recipe_id", "revision", "against"}, {"profile_base"});
            const auto base = text(q, "profile_base", 4096, "");
            const auto left = readExact(J{{"id", q.at("recipe_id")}, {"revision", q.at("revision")}}, "Recipe");
            const auto l = sideOf(left, base);
            const auto& against = q.at("against");
            Side r;
            J rightRef;
            if (against == "instrument") {
                r = instrumentSide();
                rightRef = {{"kind", "instrument"}, {"readiness_generation", r.instrument->generation}};
            } else if (against.is_object() && against.size() == 2) {
                const auto right = readExact(J{{"id", against.value("recipe_id", J(nullptr))},
                                               {"revision", against.value("revision", J(nullptr))}},
                                             "Comparison recipe");
                r = sideOf(right, base);
                rightRef = {{"kind", "recipe"}, {"id", right.document.at("id")}, {"revision", right.revision}};
            } else {
                refuse("invalid", "against must be \"instrument\" or {recipe_id, revision}");
            }
            const auto items = diff(l, r);
            return J{{"status", "ok"},
                     {"left", {{"kind", "recipe"}, {"id", left.document.at("id")}, {"revision", left.revision}}},
                     {"right", rightRef}, {"items", items}, {"overall", overall(items)},
                     {"applied", false}};
        }
        if (action == "recipe_apply_preview") {
            fields(q, {"action", "recipe_id", "revision"}, {"profile_base"});
            const auto base = text(q, "profile_base", 4096, "");
            const auto stored = readExact(J{{"id", q.at("recipe_id")}, {"revision", q.at("revision")}}, "Recipe");
            const auto l = sideOf(stored, base);
            const auto r = instrumentSide();
            const auto items = diff(l, r);
            auto statusOf = [&](const char* d) {
                for (const auto& i : items)
                    if (i.at("dimension") == d) return str(i.at("status"));
                return std::string{"unknown"};
            };
            const auto& s = *r.instrument;
            J changes = J::array(), separate = J::array(), selection = nullptr, blocked = J::array();
            const bool methodCurrent = statusOf("method") == "equal" && statusOf("profile") == "equal";
            if (!methodCurrent) {
                if (!l.facts.resolved) {
                    blocked.push_back("The pinned method cannot be resolved: " + l.facts.reason);
                } else if (l.method.at("source") == "registry") {
                    selection = {{"authority", "profile_registry"},
                                 {"command", "registry_apply_method"},
                                 {"args", {{"revisionId", l.method.at("revision")}}},
                                 {"plan_command", "registry_plan_apply"}};
                } else if (base.empty() || l.facts.localName.empty()) {
                    blocked.push_back("A local profile directory is required to select local "
                                      "profile " + str(l.method.at("id")));
                } else {
                    selection = {{"authority", "local_profile_store"},
                                 {"command", "profile_command"},
                                 {"args", {{"base", base},
                                           {"request", J{{"operation", "apply"},
                                                         {"name", l.facts.localName},
                                                         {"baseline", l.facts.contentSha256}}
                                                           .dump()}}}};
                }
                const auto via = selection.is_object() ? selection.at("command") : J(nullptr);
                changes.push_back({{"dimension", "method"},
                                   {"from", {{"source", s.methodSource}, {"id", s.methodId},
                                             {"revision", s.methodRevision}}},
                                   {"to", l.method}, {"via", via}});
                changes.push_back({{"dimension", "profile"},
                                   {"from", s.configSha256.empty() ? J(nullptr) : J(s.configSha256)},
                                   {"to", l.facts.configSha256.empty() ? J(nullptr) : J(l.facts.configSha256)},
                                   {"via", via}});
            }
            for (const auto& i : items) {
                if (i.at("dimension") == "core" && i.at("status") != "equal")
                    separate.push_back({{"dimension", "core"}, {"status", i.at("status")},
                                        {"reason", i.at("reason")},
                                        {"action", "Apply never changes the processing core; "
                                                   "select or install it through processing core "
                                                   "management"}});
                if (i.at("dimension") == "calibration" && i.at("status") != "equal")
                    separate.push_back({{"dimension", "calibration"}, {"status", i.at("status")},
                                        {"reason", i.at("reason")},
                                        {"action", "Apply never changes calibration; apply a "
                                                   "calibration record explicitly "
                                                   "(calibration_apply) or derive a new one"}});
            }
            return J{{"status", "ok"},
                     {"recipe", {{"id", stored.document.at("id")}, {"revision", stored.revision}}},
                     {"readiness_generation", s.generation},
                     {"items", items}, {"overall", overall(items)},
                     {"changes", changes}, {"method_selection", selection},
                     {"not_changed_by_apply", separate}, {"blocked", blocked},
                     {"analysis_plan", {{"id", stored.document.at("analysis_plan").at("id")},
                                        {"revision", stored.document.at("analysis_plan").at("revision")},
                                        {"effect", "Runs of this recipe are evaluated with this "
                                                   "plan revision; nothing on the instrument changes"}}},
                     {"applied", false}, {"hardware_actuated", false},
                     {"requires_explicit_acceptance", true}};
        }
        refuse("invalid", "Unsupported recipe action");
    } catch (const Refusal& r) {
        return J{{"status", r.status}, {"message", r.what()}};
    } catch (const J::exception& e) {
        return J{{"status", "invalid"}, {"message", std::string("Malformed recipe request: ") + e.what()}};
    } catch (const std::exception& e) {
        return J{{"status", "storage"}, {"message", e.what()}};
    }
}

} // namespace backend::workspace
