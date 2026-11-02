// #807 I02 (fold-in slice I-1): the native recipe provider over the real app metadata host
// (SqliteService / WorkspaceStore on a temp app.sqlite3) with stub method, analysis-plan and
// instrument authorities. Round-trip + fault-injection: immutability, derive lineage, the
// compatibility diff (equal / different / unknown with reasons), apply preview without side
// effects, namespace reservation and exact command replay.
#include "backend/database/SqliteService.h"
#include "backend/workspace/RecipeProvider.h"
#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <nlohmann/json.hpp>

#include <map>
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
