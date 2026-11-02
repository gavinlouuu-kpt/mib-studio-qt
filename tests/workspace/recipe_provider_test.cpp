// #807 I02 (fold-in slice I-1): the native recipe provider over the real app metadata host
// (SqliteService / WorkspaceStore on a temp app.sqlite3) with stub method, analysis-plan and
// instrument authorities. Round-trip + fault-injection: immutability, derive lineage, the
// compatibility diff (equal / different / unknown with reasons), apply preview without side
// effects, namespace reservation and exact command replay. #825 review regressions: a finding
// must be known to the analysis authority, adopted workspace lineage is checked (no self-parent,
// no cycle, no dangling reference), a refused derive writes nothing, and a method content hash
// change under one revision is a method difference.
#include "backend/database/SqliteService.h"
#include "backend/workspace/RecipeProvider.h"
#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <nlohmann/json.hpp>

#include <map>
#include <set>
#include <string>

using nlohmann::json;
using backend::workspace::RecipeInstrumentState;
using backend::workspace::RecipeMethodFacts;

namespace {

struct World {
    backend::services::SqliteService db;
    // Method authority: key "source:id:revision".
    std::map<std::string, RecipeMethodFacts> methods;
    // Analysis authority: key "id@revision" -> document.
    std::map<std::string, json> plans;
    // Finding authority (analysis readFinding): keys "id@revision".
    std::set<std::string> findings;
    RecipeInstrumentState instrument;
    int instrumentReads{0};
    int storeWrites{0};

    backend::workspace::RecipeProvider provider() {
        backend::workspace::RecipeHost host;
        host.store = [this](const json& q, bool write) {
            if (write) ++storeWrites;
            return json::parse(db.recipeStoreRequest(q.dump(), write));
        };
        host.method = [this](const json& ref, const std::string&) {
            const auto key = ref.at("source").get<std::string>() + ":" + ref.at("id").get<std::string>() +
                             ":" + ref.at("revision").get<std::string>();
            const auto it = methods.find(key);
            if (it != methods.end()) return it->second;
            RecipeMethodFacts f;
            f.reason = "not in the stub method authority";
            return f;
        };
        host.plan = [this](const std::string& id, const std::string& revision) {
            const auto it = plans.find(id + "@" + revision);
            if (it == plans.end()) return json{{"status", "not_found"}, {"message", "no such plan"}};
            return json{{"status", "ok"}, {"planId", id}, {"revision", revision}, {"document", it->second}};
        };
        host.finding = [this](const std::string& id, const std::string& revision) {
            if (!findings.count(id + "@" + revision))
                return json{{"status", "not_found"}, {"message", "no such finding"}};
            return json{{"status", "ok"}, {"findingId", id}, {"revision", revision},
                        {"document", {{"type", "F-analysis-finding/1"}}}};
        };
        host.instrument = [this] {
            ++instrumentReads;
            return instrument;
        };
        host.now = [] { return std::string("2026-11-02T10:00:00.000Z"); };
        return backend::workspace::RecipeProvider(host);
    }
};

RecipeMethodFacts local(const std::string& revision, const std::string& config, std::optional<int> contract,
                        const std::string& policy = "none") {
    RecipeMethodFacts f;
    f.resolved = true;
    f.provider = "local-profile";
    f.contentSha256 = revision;
    f.configSha256 = config;
    f.contractVersion = contract;
    if (!contract) f.coreReason = "the local profile declares no processing contract and pins no core build";
    f.calibrationPolicy = policy;
    f.localName = "beads-dir";
    return f;
}

json status(const json& items, const std::string& dimension) {
    for (const auto& i : items)
        if (i.at("dimension") == dimension) return i;
    return json(nullptr);
}

} // namespace

int main() {
    mib::test::Watchdog watchdog(30);
    mib::test::TempDir temp("recipe_provider");
    World w;
    MIB_REQUIRE(w.db.initialize((temp.path() / "app.sqlite3").string()), "metadata host");

    w.methods["local:beads:rev-a"] = local("rev-a", "cfg-a", 2);
    w.methods["local:cells:rev-b"] = local("rev-b", "cfg-b", std::nullopt, "required");
    w.plans["plan-1@1"] = json{{"type", "F-analysis-strategy/1"}, {"method", nullptr}};
    w.plans["plan-1@2"] = json{{"type", "F-analysis-strategy/1"}};
    w.findings.insert("finding-1@1");
    w.plans["bound@1"] = json{{"method", {{"method_id", "other"}, {"revision_id", "x"}}}};
    w.instrument.generation = "7";
    w.instrument.methodSource = "local";
    w.instrument.methodId = "beads";
    w.instrument.methodRevision = "rev-a";
    w.instrument.methodNamed = true;
    w.instrument.configSha256 = "cfg-a";
    w.instrument.coreVersion = "1.4.0";
    w.instrument.contractVersion = 2;
    w.instrument.calibrationRecordId = std::string("cal-1");
    w.instrument.lutLoaded = true;
    w.instrument.lutSha256 = "lut-1";

    auto p = w.provider();
    auto call = [&](const json& q, bool write = true) { return p.handle(q, write, false); };
    const json create{{"action", "recipe_create"},
                      {"command_id", "cmd-create"},
                      {"recipe_id", "r-original"},
                      {"title", "Bead QC"},
                      {"note", "baseline"},
                      {"method", {{"source", "local"}, {"id", "beads"}, {"revision", "rev-a"}}},
                      {"analysis_plan", {{"id", "plan-1"}, {"revision", "1"}}},
                      {"profile_base", "/profiles"}};

    // --- validation and refusals -------------------------------------------------------------
    MIB_EXPECT(call(create, false)["status"] == "invalid", "read endpoint refuses create");
    auto bad = create;
    bad["extra"] = 1;
    MIB_EXPECT(call(bad)["status"] == "invalid", "unknown fields refused");
    bad = create;
    bad["recipe_id"] = "../escape";
    MIB_EXPECT(call(bad)["status"] == "invalid", "identity syntax enforced");
    bad = create;
    bad["method"]["revision"] = "missing";
    auto refused = call(bad);
    MIB_EXPECT(refused["status"] == "unavailable" &&
                   refused["message"].get<std::string>().find("not in the stub") != std::string::npos,
               "unresolved method refused with the authority's reason: " + refused.dump());
    bad = create;
    bad["analysis_plan"]["revision"] = "9";
    MIB_EXPECT(call(bad)["status"] == "not_found", "missing analysis plan revision refused");
    bad = create;
    bad["analysis_plan"] = {{"id", "bound"}, {"revision", "1"}};
    MIB_EXPECT(call(bad)["status"] == "invalid", "plan bound to another method refused");
    MIB_EXPECT(w.storeWrites == 0, "no refused request wrote anything");

    // --- create, replay, immutability --------------------------------------------------------
    const auto created = call(create);
    MIB_REQUIRE(created["status"] == "ok", created.dump());
    const auto original = created["recipe"];
    MIB_EXPECT(original["revision"] == "1" && original["lineage"]["recipe"].is_null(), "fresh recipe");
    MIB_EXPECT(original["method"]["content_sha256"] == "rev-a", "records the method content hash");
    MIB_EXPECT(original["basis"]["calibration_record_id"] == "cal-1" &&
                   original["basis"]["emodulus_lut"]["sha256"] == "lut-1",
               "records the calibration basis it was created under");
    const auto replay = call(create);
    MIB_EXPECT(replay["replayed"] == true && replay["recipe"] == original, "exact retry replays");
    bad = create;
    bad["note"] = "changed";
    MIB_EXPECT(call(bad)["status"] == "conflict", "an existing recipe id is never rewritten");
    bad["command_id"] = "cmd-other";
    MIB_EXPECT(call(bad)["status"] == "conflict", "a new command cannot overwrite either");
    auto storeRaw = [&](const json& q, bool write) { return json::parse(w.db.workspaceRequest(q.dump(), write)); };
    MIB_EXPECT(storeRaw({{"action", "commit"}, {"workspace_id", "recipe/r-original"},
                         {"expected_revision", "1"}, {"command_id", "spoof"},
                         {"document", {{"schema_version", 1}}}},
                        true)["status"] == "invalid",
               "public workspace_write cannot revise a recipe");
    MIB_EXPECT(storeRaw({{"action", "commit"}, {"workspace_id", "recipe/new"},
                         {"expected_revision", nullptr}, {"command_id", "spoof"},
                         {"document", {{"schema_version", 1}}}},
                        true)["status"] == "invalid",
               "public workspace_write cannot mint a recipe");
    MIB_EXPECT(storeRaw({{"action", "restore"}, {"workspace_id", "recipe/r-original"},
                         {"expected_revision", "1"}, {"command_id", "spoof"}, {"target_revision", "1"}},
                        true)["status"] == "invalid",
               "public restore refused in the recipe namespace");
    MIB_EXPECT(json::parse(w.db.recipeStoreRequest(
                   json{{"action", "commit"}, {"workspace_id", "recipe/r-original"},
                        {"expected_revision", "1"}, {"command_id", "x"}, {"document", {{"schema_version", 1}}}}
                       .dump(),
                   true))["status"] == "invalid",
               "even the recipe authority is create-only");
    MIB_EXPECT(json::parse(w.db.recipeStoreRequest(
                   json{{"action", "commit"}, {"workspace_id", "plain"}, {"expected_revision", nullptr},
                        {"command_id", "x"}, {"document", {{"schema_version", 1}}}}
                       .dump(),
                   true))["status"] == "invalid",
               "the recipe authority writes only its namespace");

    // --- derive (base on previous) ------------------------------------------------------------
    const json derive{{"action", "recipe_derive"},
                      {"command_id", "cmd-derive"},
                      {"recipe_id", "r-next"},
                      {"parent", {{"id", "r-original"}, {"revision", "1"}}},
                      {"analysis_plan", {{"id", "plan-1"}, {"revision", "2"}}},
                      {"finding", {{"id", "finding-1"}, {"revision", "1"}}},
                      {"note", "tighter gate"}};
    bad = derive;
    bad["recipe_id"] = "r-original";
    MIB_EXPECT(call(bad)["status"] == "invalid", "derive needs a fresh identity");
    bad = derive;
    bad["parent"]["revision"] = "2";
    MIB_EXPECT(call(bad)["status"] == "not_found", "derive from a missing parent revision refused");
    bad = derive;
    bad["parent_document"] = json::object();
    MIB_EXPECT(call(bad)["status"] == "invalid", "parent adoption is in-process only");
    const auto derived = call(derive);
    MIB_REQUIRE(derived["status"] == "ok", derived.dump());
    const auto next = derived["recipe"];
    MIB_EXPECT(next["lineage"]["recipe"] == json({{"id", "r-original"}, {"revision", "1"}}) &&
                   next["lineage"]["finding"] == json({{"id", "finding-1"}, {"revision", "1"}}),
               "derived recipe links parent and finding");
    MIB_EXPECT(next["method"] == original["method"] && next["analysis_plan"]["revision"] == "2" &&
                   next["title"] == "Bead QC",
               "derive keeps unchanged references and replaces the declared ones");
    MIB_EXPECT(call(derive)["recipe"] == next, "derive replays exactly");
    const auto reread = call({{"action", "recipe_get"}, {"recipe_id", "r-original"}, {"revision", "1"}}, false);
    MIB_EXPECT(reread["recipe"] == original, "the original is byte-for-byte unchanged after derive");
    MIB_EXPECT(storeRaw({{"action", "history"}, {"workspace_id", "recipe/r-original"}}, false)["revisions"] ==
                   json::array({"1"}),
               "the original still has exactly one revision");

    // --- list ---------------------------------------------------------------------------------
    const auto page1 = call({{"action", "recipe_list"}, {"limit", 1}}, false);
    MIB_EXPECT(page1["recipes"].size() == 1 && page1["next_after"] == "r-next", page1.dump());
    const auto page2 = call({{"action", "recipe_list"}, {"limit", 1}, {"after", "r-next"}}, false);
    MIB_EXPECT(page2["recipes"].size() == 1 && page2["recipes"][0]["id"] == "r-original" &&
                   page2["next_after"].is_null(),
               page2.dump());

    // --- diff: equal / different / unknown, each with a reason --------------------------------
    auto diff = [&](const json& against, const std::string& id = "r-original") {
        return call({{"action", "recipe_diff"}, {"recipe_id", id}, {"revision", "1"}, {"against", against}}, false);
    };
    auto d = diff("instrument");
    MIB_REQUIRE(d["status"] == "ok", d.dump());
    MIB_EXPECT(d["overall"] == "equal", d.dump());
    for (const auto& i : d["items"])
        MIB_EXPECT(i["status"] == "equal" && !i["reason"].get<std::string>().empty(), i.dump());
    MIB_EXPECT(d["right"]["readiness_generation"] == "7", "diff names the instrument facts it used");

    w.instrument.methodRevision = "rev-z";
    w.instrument.configSha256 = "cfg-z";
    w.instrument.contractVersion = 3;
    w.instrument.calibrationRecordId = std::nullopt;
    w.instrument.lutSha256 = "lut-2";
    d = diff("instrument");
    MIB_EXPECT(d["overall"] == "different", d.dump());
    for (const auto* dim : {"method", "profile", "core", "calibration"})
        MIB_EXPECT(status(d["items"], dim)["status"] == "different", std::string(dim) + " " + d.dump());
    MIB_EXPECT(status(d["items"], "calibration")["reason"].get<std::string>().find("no record in effect") !=
                   std::string::npos,
               "calibration reason names the record change");

    w.instrument.methodNamed = false;
    w.instrument.calibrationKnown = false;
    w.instrument.calibrationReason = "the calibration record store is unreadable: corrupt";
    d = diff("instrument");
    MIB_EXPECT(status(d["items"], "method")["status"] == "unknown" &&
                   status(d["items"], "method")["reason"].get<std::string>().find("does not name") !=
                       std::string::npos,
               "an unnamed applied config is unknown, with why");
    MIB_EXPECT(status(d["items"], "calibration")["status"] == "unknown" &&
                   status(d["items"], "calibration")["reason"].get<std::string>().find("corrupt") !=
                       std::string::npos,
               "an unreadable calibration store is unknown, with why");
    w.instrument.methodSource = "none";
    d = diff("instrument");
    MIB_EXPECT(status(d["items"], "method")["status"] == "different", "no applied method is a difference, not unknown");

    // Recipe vs recipe: a method the authority no longer resolves makes profile/core unknown.
    w.instrument.calibrationKnown = true; // r-cells records the same basis as r-original
    w.instrument.calibrationRecordId = std::string("cal-1");
    w.instrument.lutSha256 = "lut-1";
    MIB_REQUIRE(call({{"action", "recipe_create"}, {"command_id", "c3"}, {"recipe_id", "r-cells"},
                      {"title", "Cells"}, {"method", {{"source", "local"}, {"id", "cells"}, {"revision", "rev-b"}}},
                      {"analysis_plan", {{"id", "plan-1"}, {"revision", "1"}}}})["status"] == "ok",
                "second recipe");
    d = diff({{"recipe_id", "r-next"}, {"revision", "1"}});
    MIB_EXPECT(d["overall"] == "equal", "parent and child share method, profile, core, calibration: " + d.dump());
    d = diff({{"recipe_id", "r-cells"}, {"revision", "1"}});
    MIB_EXPECT(status(d["items"], "method")["status"] == "different" &&
                   status(d["items"], "profile")["status"] == "different",
               d.dump());
    MIB_EXPECT(status(d["items"], "core")["status"] == "unknown" &&
                   status(d["items"], "core")["reason"].get<std::string>().find("declares no processing contract") !=
                       std::string::npos,
               "an undeclared core is unknown with the method's reason: " + d.dump());
    MIB_EXPECT(status(d["items"], "calibration")["status"] == "different" &&
                   status(d["items"], "calibration")["reason"].get<std::string>().find("policy none vs required") !=
                       std::string::npos,
               "same basis, different calibration policy: " + d.dump());
    w.methods.erase("local:cells:rev-b");
    d = diff({{"recipe_id", "r-cells"}, {"revision", "1"}});
    MIB_EXPECT(status(d["items"], "profile")["status"] == "unknown" &&
                   status(d["items"], "profile")["reason"].get<std::string>().find("not resolved") != std::string::npos,
               "unresolvable method: profile unknown with reason");
    w.methods["local:cells:rev-b"] = local("rev-b", "cfg-b", std::nullopt, "required");
    w.methods["local:beads:rev-a"].contentSha256 = "tampered";
    d = diff("instrument");
    MIB_EXPECT(status(d["items"], "profile")["status"] == "unknown" &&
                   status(d["items"], "profile")["reason"].get<std::string>().find("differs from the hash") !=
                       std::string::npos,
               "content changed under the same revision is detected");
    w.methods["local:beads:rev-a"].contentSha256 = "rev-a";

    // --- apply preview: exact changes, no side effects ----------------------------------------
    w.instrument.methodSource = "local";
    w.instrument.methodNamed = true;
    w.instrument.calibrationKnown = true;
    w.instrument.calibrationRecordId = std::nullopt; // calibration differs from the recipe's basis
    const auto writesBefore = w.storeWrites;
    const auto listBefore = call({{"action", "recipe_list"}}, false);
    const auto instrumentBefore = w.instrument.methodRevision;
    const json preview{{"action", "recipe_apply_preview"}, {"recipe_id", "r-original"}, {"revision", "1"},
                       {"profile_base", "/profiles"}};
    const auto a = call(preview, false);
    MIB_REQUIRE(a["status"] == "ok", a.dump());
    MIB_EXPECT(a["applied"] == false && a["hardware_actuated"] == false && a["requires_explicit_acceptance"] == true,
               "preview never applies or actuates");
    MIB_EXPECT(a["method_selection"]["command"] == "profile_command" &&
                   json::parse(a["method_selection"]["args"]["request"].get<std::string>()) ==
                       json({{"operation", "apply"}, {"name", "beads-dir"}, {"baseline", "rev-a"}}),
               "preview names the exact method-authority request: " + a.dump());
    MIB_EXPECT(a["changes"].size() == 2 && a["changes"][0]["to"]["revision"] == "rev-a" &&
                   a["changes"][1]["to"] == "cfg-a",
               "preview lists the method and profile change");
    bool core = false, calibration = false;
    for (const auto& s : a["not_changed_by_apply"]) {
        core |= s["dimension"] == "core";
        calibration |= s["dimension"] == "calibration";
    }
    MIB_EXPECT(core && calibration, "core and calibration differences are never changed by apply");
    MIB_EXPECT(w.storeWrites == writesBefore && call({{"action", "recipe_list"}}, false) == listBefore &&
                   w.instrument.methodRevision == instrumentBefore,
               "preview wrote nothing and changed no instrument state");
    auto noBase = preview;
    noBase.erase("profile_base");
    w.methods["local:beads:rev-a"].localName.clear();
    MIB_EXPECT(call(noBase, false)["blocked"].size() == 1, "a local method without its directory is blocked, not guessed");
    w.methods["local:beads:rev-a"].localName = "beads-dir";
    w.instrument.methodRevision = "rev-a";
    w.instrument.configSha256 = "cfg-a";
    const auto current = call(preview, false);
    MIB_EXPECT(current["method_selection"].is_null() && current["changes"].empty(),
               "an already-applied method needs no selection");
    MIB_EXPECT(call({{"action", "recipe_get"}, {"recipe_id", "r-original"}, {"revision", "1"}}, false)["recipe"] == original,
               "after create, derive, diff and apply preview the original is unchanged");

    // --- #825 review: findings, adoption lineage, refused derives, method content ---------------
    auto catalog = [&] { return call({{"action", "recipe_list"}, {"limit", 100}}, false); };
    auto has = [](const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; };
    auto message = [](const json& r) { return r.value("message", std::string{}); };
    {
        // M2: a public derive names a finding the analysis authority must know.
        const auto before = catalog();
        const auto writes = w.storeWrites;
        auto bogus = derive;
        bogus["command_id"] = "cmd-bogus";
        bogus["recipe_id"] = "r-bogus";
        bogus["finding"] = {{"id", "bogus"}, {"revision", "77"}};
        const auto r = call(bogus);
        MIB_EXPECT(r["status"] == "not_found" && has(message(r), "bogus @ 77") &&
                       has(message(r), "not known to the analysis authority"),
                   "a bogus finding is refused: " + r.dump());
        MIB_EXPECT(catalog() == before && w.storeWrites == writes, "the refused derive wrote nothing");
    }
    // In-process adoption (NextRunPlanning draft_from_finding) of a user-written workspace recipe.
    auto workspaceRecipe = [](const std::string& id, const json& lineageRecipe, const json& lineageFinding) {
        return json{{"schema_version", 1}, {"id", id}, {"revision", "1"}, {"title", "Workspace " + id},
                    {"question", "why " + id},
                    {"method", {{"source", "local"}, {"id", "beads"}, {"revision", "rev-a"}}},
                    {"analysis_plan", {{"id", "plan-1"}, {"revision", "1"}}},
                    {"lineage", {{"recipe", lineageRecipe}, {"finding", lineageFinding}}}};
    };
    auto adoptDerive = [&](const std::string& child, const json& doc) {
        return p.handle({{"action", "recipe_derive"}, {"command_id", "cmd-" + child}, {"recipe_id", child},
                         {"parent", {{"id", doc.at("id")}, {"revision", "1"}}}, {"parent_document", doc}},
                        true, true);
    };
    auto get = [&](const std::string& id) {
        return call({{"action", "recipe_get"}, {"recipe_id", id}, {"revision", "1"}}, false);
    };
    auto ref = [](const std::string& id) { return json{{"id", id}, {"revision", "1"}}; };
    {
        // M1: self-parent and the id being derived are refused; nothing is written.
        const auto before = catalog();
        const auto writes = w.storeWrites;
        auto r = adoptDerive("child-c", workspaceRecipe("ws-c", ref("ws-c"), nullptr));
        MIB_EXPECT(r["status"] == "invalid" && has(message(r), "names itself as its parent"),
                   "a self-parent is refused: " + r.dump());
        r = adoptDerive("child-d", workspaceRecipe("ws-d", ref("child-d"), nullptr));
        MIB_EXPECT(r["status"] == "invalid" && has(message(r), "names the recipe being derived"),
                   "the id being derived as parent is refused: " + r.dump());
        MIB_EXPECT(catalog() == before && w.storeWrites == writes && get("ws-c")["status"] == "not_found" &&
                       get("ws-d")["status"] == "not_found",
                   "refused adoptions wrote nothing");

        // A cycle ws-a <-> ws-b cannot be adopted: ws-b is no provider recipe when ws-a is adopted.
        r = adoptDerive("child-a", workspaceRecipe("ws-a", ref("ws-b"), nullptr));
        MIB_REQUIRE(r["status"] == "ok", r.dump());
        auto a = get("ws-a")["recipe"];
        MIB_EXPECT(a["lineage"]["recipe"].is_null() && a["adoption_notes"].size() == 1 &&
                       has(a["adoption_notes"][0].get<std::string>(),
                           "lineage not adopted: parent not in provider (ws-b @ 1)"),
                   "a parent outside the provider is dropped with the reason: " + a.dump());
        MIB_EXPECT(a["method"]["content_sha256"] == "rev-a", "the adopted pair was validated: " + a.dump());
        r = adoptDerive("child-b", workspaceRecipe("ws-b", ref("ws-a"), nullptr));
        MIB_REQUIRE(r["status"] == "ok", r.dump());
        const auto b = get("ws-b")["recipe"];
        MIB_EXPECT(b["lineage"]["recipe"] == ref("ws-a") && b["adoption_notes"].empty(),
                   "a provider parent is adopted: " + b.dump());
        std::set<std::string> seen;
        std::string at = "child-b";
        while (!at.empty() && seen.insert(at).second) {
            const auto node = get(at);
            MIB_REQUIRE(node["status"] == "ok", "lineage resolves in the provider: " + at + " " + node.dump());
            const auto& parent = node["recipe"]["lineage"]["recipe"];
            at = parent.is_null() ? std::string{} : parent["id"].get<std::string>();
        }
        MIB_EXPECT(at.empty() && seen == std::set<std::string>({"child-b", "ws-b", "ws-a"}),
                   "provider lineage ends without a cycle");

        // A dangling finding is dropped with the reason; a known one is kept.
        r = adoptDerive("child-e", workspaceRecipe("ws-e", nullptr, ref("ghost")));
        MIB_REQUIRE(r["status"] == "ok", r.dump());
        const auto e = get("ws-e")["recipe"];
        MIB_EXPECT(e["lineage"]["finding"].is_null() && e["adoption_notes"].size() == 1 &&
                       has(e["adoption_notes"][0].get<std::string>(), "lineage not adopted: finding ghost @ 1"),
                   "a dangling finding is dropped with the reason: " + e.dump());
        r = adoptDerive("child-f", workspaceRecipe("ws-f", nullptr, ref("finding-1")));
        MIB_REQUIRE(r["status"] == "ok", r.dump());
        MIB_EXPECT(get("ws-f")["recipe"]["lineage"]["finding"] == ref("finding-1"), "a known finding is kept");
    }
    {
        // m1: a derive refused after adoption validated writes nothing, the parent included.
        const auto before = catalog();
        const auto writes = w.storeWrites;
        auto q = json{{"action", "recipe_derive"}, {"command_id", "cmd-child-g"}, {"recipe_id", "child-g"},
                      {"parent", ref("ws-g")}, {"analysis_plan", {{"id", "plan-1"}, {"revision", "9"}}},
                      {"parent_document", workspaceRecipe("ws-g", nullptr, nullptr)}};
        auto r = p.handle(q, true, true);
        MIB_EXPECT(r["status"] == "not_found", "the child's plan is refused: " + r.dump());
        auto unresolved = workspaceRecipe("ws-h", nullptr, nullptr);
        unresolved["method"]["revision"] = "missing";
        r = adoptDerive("child-h", unresolved);
        MIB_EXPECT(r["status"] == "unavailable", "the adopted pair is validated: " + r.dump());
        MIB_EXPECT(catalog() == before && w.storeWrites == writes && get("ws-g")["status"] == "not_found" &&
                       get("ws-h")["status"] == "not_found",
                   "refused derives wrote nothing, not even the adopted parent");
    }
    {
        // m2: the same method revision with another content hash is a different method.
        const json same{{"source", "local"}, {"id", "beads"}, {"revision", "rev-a"}};
        auto make = [&](const std::string& id, const json& method) {
            return call({{"action", "recipe_create"}, {"command_id", "cmd-" + id}, {"recipe_id", id},
                         {"title", id}, {"method", method}, {"analysis_plan", {{"id", "plan-1"}, {"revision", "1"}}}});
        };
        MIB_REQUIRE(make("r-h1", same)["status"] == "ok", "r-h1");
        w.methods["local:beads:rev-a"].contentSha256 = "rev-a-v2";
        MIB_REQUIRE(make("r-h2", same)["status"] == "ok", "r-h2");
        auto d2 = diff({{"recipe_id", "r-h2"}, {"revision", "1"}}, "r-h1");
        auto m = status(d2["items"], "method");
        MIB_EXPECT(m["status"] == "different" && has(m["reason"].get<std::string>(), "method content changed for this revision") &&
                       m["left"]["content_sha256"] == "rev-a" && m["right"]["content_sha256"] == "rev-a-v2",
                   "recipe vs recipe: content differs under one revision: " + d2.dump());
        d2 = diff("instrument", "r-h1");
        m = status(d2["items"], "method");
        MIB_EXPECT(m["status"] == "different" && has(m["reason"].get<std::string>(), "method content changed for this revision") &&
                       m["right"]["content_sha256"] == "rev-a-v2",
                   "recipe vs instrument: the authority's content changed: " + d2.dump());
        d2 = diff("instrument", "r-h2");
        MIB_EXPECT(status(d2["items"], "method")["status"] == "equal", "the current content is equal: " + d2.dump());
        w.methods["local:beads:rev-a"].contentSha256 = "rev-a";
        w.methods["local:nohash:n1"] = local("", "cfg-a", 2);
        const json nohash{{"source", "local"}, {"id", "nohash"}, {"revision", "n1"}};
        MIB_REQUIRE(make("r-n1", nohash)["status"] == "ok" && make("r-n2", nohash)["status"] == "ok", "no-hash recipes");
        d2 = diff({{"recipe_id", "r-n2"}, {"revision", "1"}}, "r-n1");
        m = status(d2["items"], "method");
        MIB_EXPECT(m["status"] == "unknown" && has(m["reason"].get<std::string>(), "recorded no method content hash"),
                   "a missing hash is unknown, with why: " + d2.dump());
    }

    // --- restart: recipes survive and stay immutable ------------------------------------------
    {
        World again;
        MIB_REQUIRE(again.db.initialize((temp.path() / "app.sqlite3").string()), "reopen");
        again.methods = w.methods;
        again.plans = w.plans;
        auto q = again.provider();
        MIB_EXPECT(q.handle({{"action", "recipe_get"}, {"recipe_id", "r-next"}, {"revision", "1"}}, false, false)["recipe"] == next,
                   "derived recipe survives restart");
        MIB_EXPECT(q.handle(create, true, false)["replayed"] == true, "replay after restart");
    }
    return mib::test::exitCode();
}
