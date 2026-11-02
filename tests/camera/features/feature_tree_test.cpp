// E02 (E-1/E-2): node-map enumeration and guarded typed access over a fake port.
// No production feature name appears here: every node name is generated, so the
// tree the service returns can only have come from the port.
#include "backend/camera/features/FakeNodeMap.h"
#include "backend/camera/features/FeatureTree.h"
#include "backend/camera/features/ModuleWriteTracker.h"

#include <cstdlib>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace camera::features;
namespace {
int checks = 0;
void check(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

// A session guard double: counts guarded steps, can move the session after N
// steps (stop/restart or a write elsewhere), or close/lack the port.
struct TestGuard : GuardedPort {
    Port& port;
    CurrentSession current;
    Access mode{Access::Ran};
    int steps{0};
    int bumpGenerationAfter{-1};
    int refuseAfter{-1}; // refuse (trigger armed, run started) from this step on
    explicit TestGuard(Port& p) : port(p) {
        current.token = {"source-under-test", 41, 1};
        current.writesAuthorized = true;
        current.evidence = "fixture/mock";
    }
    Access run(const std::function<void(Port&, CurrentSession&)>& fn,
               std::string& reason) override {
        if (mode != Access::Ran) {
            reason = mode == Access::Unsupported ? "no node map"
                     : mode == Access::Refused   ? "trigger armed"
                                                 : "no session";
            return mode;
        }
        if (refuseAfter >= 0 && steps >= refuseAfter) {
            reason = "trigger armed";
            return Access::Refused;
        }
        if (bumpGenerationAfter >= 0 && steps == bumpGenerationAfter) ++current.token.generation;
        ++steps;
        fn(port, current);
        return Access::Ran;
    }
};

// Delegating port that injects SDK failures by name.
struct FaultyPort : Port {
    FakeNodeMap& inner;
    std::set<std::string> failFeaturesOf, failDescribe;
    bool failCategories{false};
    explicit FaultyPort(FakeNodeMap& m) : inner(m) {}
    Descriptor describe(const Address& a) override {
        if (failDescribe.count(a.feature)) throw std::runtime_error("describe transport error");
        return inner.describe(a);
    }
    void write(const Address& a, const Value& v) override { inner.write(a, v); }
    bool canEnumerate() const override { return true; }
    std::vector<std::string> categories(Module m, bool o) override {
        if (failCategories) throw std::runtime_error("categories transport error");
        return inner.categories(m, o);
    }
    std::vector<std::string> categoriesOf(Module m, const std::string& c, bool o) override {
        return inner.categoriesOf(m, c, o);
    }
    std::vector<std::string> featuresOf(Module m, const std::string& c, bool o) override {
        if (failFeaturesOf.count(c)) throw std::runtime_error("featuresOf transport error");
        return inner.featuresOf(m, c, o);
    }
    std::vector<std::string> features(Module m, bool o) override { return inner.features(m, o); }
    NodeInfo info(Module m, const std::string& n) override { return inner.info(m, n); }
};

struct PlainPort : Port { // no introspection
    Descriptor describe(const Address&) override { return {}; }
    void write(const Address&, const Value&) override {}
};

// The expected shape, recorded while generating the node map.
struct Expected {
    std::vector<std::string> categoryOrder, roots, featureOrder, uncategorized;
    std::map<std::string, std::vector<std::string>> childCategories, childFeatures;
};

std::string name(std::mt19937& rng, const char* stem, int i) {
    return std::string(stem) + std::to_string(i) + "_" + std::to_string(rng() % 100000);
}

// Random tree: categories under random earlier parents, typed features in random
// categories (some outside every category).
Expected populate(FakeNodeMap& map, Module module, std::mt19937& rng, int categories,
                  int features) {
    Expected e;
    for (int i = 0; i < categories; ++i) {
        const auto cat = name(rng, "C", i);
        const bool root = i == 0 || rng() % 4 == 0;
        const auto parent = root ? std::string() : e.categoryOrder[rng() % e.categoryOrder.size()];
        map.addCategory(module, cat, NodeInfo{"Display " + cat, "Tip " + cat, "", "Beginner"}, parent);
        e.categoryOrder.push_back(cat);
        e.childCategories[cat];
        e.childFeatures[cat];
        if (root)
            e.roots.push_back(cat);
        else
            e.childCategories[parent].push_back(cat);
    }
    for (int i = 0; i < features; ++i) {
        FakeNode n;
        n.name = name(rng, "F", i);
        n.info = NodeInfo{"Display " + n.name, "", "Desc " + n.name, i % 2 ? "Expert" : "Guru"};
        switch (i % 5) {
        case 0:
            n.kind = Kind::Integer;
            n.value = std::int64_t{i};
            n.minimum = std::int64_t{0};
            n.maximum = std::int64_t{1000};
            n.increment = std::int64_t{1};
            break;
        case 1:
            n.kind = Kind::Float;
            n.value = 0.5 * i;
            n.minimum = 0.0;
            n.maximum = 1e6;
            n.unit = "u" + std::to_string(i);
            break;
        case 2:
            n.kind = Kind::Boolean;
            n.value = i % 3 == 0;
            break;
        case 3:
            n.kind = Kind::Enumeration;
            n.entries = {name(rng, "E", 0), name(rng, "E", 1)};
            n.value = n.entries.front();
            break;
        default:
            n.kind = Kind::String;
            n.value = name(rng, "S", i);
            n.writable = false;
            break;
        }
        const bool outside = rng() % 6 == 0;
        const auto cat = outside ? std::string() : e.categoryOrder[rng() % e.categoryOrder.size()];
        map.addFeature(module, n, cat);
        if (outside)
            e.uncategorized.push_back(n.name);
        else
            e.childFeatures[cat].push_back(n.name);
    }
    // Service order: category traversal (node-map category order), then the rest.
    std::set<std::string> seen;
    for (const auto& c : e.categoryOrder)
        for (const auto& f : e.childFeatures[c])
            if (seen.insert(f).second) e.featureOrder.push_back(f);
    for (const auto& f : e.uncategorized) e.featureOrder.push_back(f);
    return e;
}

void enumerationMatchesTheNodeMapExactly() {
    std::mt19937 rng(806);
    FakeNodeMap map;
    const auto remote = populate(map, Module::Remote, rng, 9, 40);
    const auto stream = populate(map, Module::Stream, rng, 3, 7);
    TestGuard guard(map);
    TreeOptions options;
    options.modules = {Module::Remote, Module::Stream, Module::Interface};
    const auto tree = enumerate(guard, options);
    check(tree.outcome == Outcome::Read, "tree read");
    check(tree.session.generation == 41 && tree.session.capabilityRevision == 1,
          "tree carries the session token");
    check(tree.evidence == "fixture/mock", "tree carries the session evidence class");
    check(tree.modules.size() == 3, "one entry per requested module");
    const std::pair<const Expected*, const ModuleTree*> pairs[] = {{&remote, &tree.modules[0]},
                                                                   {&stream, &tree.modules[1]}};
    for (const auto& [e, m] : pairs) {
        check(m->enumerated && m->reason.empty() && !m->truncated, "module enumerated");
        check(m->roots == e->roots, "roots are the categories without a parent");
        check(m->categories.size() == e->categoryOrder.size(), "every category, once");
        for (std::size_t i = 0; i < m->categories.size(); ++i) {
            const auto& c = m->categories[i];
            check(c.name == e->categoryOrder[i], "category order is node-map order");
            check(c.categories == e->childCategories.at(c.name), "child categories exact");
            check(c.features == e->childFeatures.at(c.name), "child features exact");
            check(c.info.displayName == "Display " + c.name && c.info.toolTip == "Tip " + c.name,
                  "category info from the node map");
            check(c.error.empty(), "no category error");
        }
        check(m->uncategorized == e->uncategorized, "features outside every category kept");
        check(m->features.size() == e->featureOrder.size(), "every feature, once");
        for (std::size_t i = 0; i < m->features.size(); ++i) {
            const auto& f = m->features[i];
            check(f.name == e->featureOrder[i], "feature order");
            check(f.descriptor.has_value() && f.error.empty(), "feature described");
            const auto direct = map.describe(Address{m->module, f.name, std::nullopt});
            check(f.descriptor->kind == direct.kind && f.descriptor->value == direct.value &&
                      f.descriptor->writable == direct.writable &&
                      f.descriptor->minimum == direct.minimum &&
                      f.descriptor->maximum == direct.maximum &&
                      f.descriptor->entries == direct.entries && f.descriptor->unit == direct.unit,
                  "descriptor is the port's own description");
            check(f.info.displayName == "Display " + f.name &&
                      f.info.description == "Desc " + f.name && !f.info.visibility.empty(),
                  "feature info from the node map");
        }
    }
    // Interface is not in this node map: absent, never an empty enumerated module.
    check(!tree.modules[2].enumerated && tree.modules[2].categories.empty() &&
              tree.modules[2].features.empty() && tree.modules[2].roots.empty(),
          "an absent module is not enumerated");
    check(tree.modules[2].reason.rfind("Absent: ", 0) == 0 &&
              tree.modules[2].reason.find("interface") != std::string::npos,
          "an absent module says it is absent");
    // Per-node guard steps: the guard is never held across the whole walk.
    const int nodes = 9 + 40 + 3 + 7;
    check(guard.steps >= nodes, "one guarded step per node");

    // Without descriptors only names and info are read.
    TreeOptions light;
    light.modules = {Module::Remote};
    light.descriptors = false;
    const auto names = enumerate(guard, light);
    check(names.outcome == Outcome::Read && !names.modules[0].features.empty() &&
              !names.modules[0].features[0].descriptor,
          "descriptors are optional");
}

void enumerationRefusesStaleAndUnsupported() {
    FakeNodeMap map;
    std::mt19937 rng(1);
    populate(map, Module::Remote, rng, 4, 12);
    {
        TestGuard guard(map);
        guard.bumpGenerationAfter = 6; // the session moves mid-walk
        const auto tree = enumerate(guard, {});
        check(tree.outcome == Outcome::Stale && tree.modules.empty(),
              "a session change mid-walk is stale and nothing partial is current");
    }
    {
        TestGuard guard(map);
        guard.mode = Access::Unsupported;
        const auto tree = enumerate(guard, {});
        check(tree.outcome == Outcome::Unsupported && tree.reason == "no node map",
              "no GenApi: unsupported, not empty");
    }
    {
        TestGuard guard(map);
        guard.mode = Access::NoSession;
        check(enumerate(guard, {}).outcome == Outcome::Refused, "no session: refused");
    }
    {
        // The guard refuses before the port (the sort trigger is armed): refused
        // with the guard's reason, nothing enumerated.
        TestGuard guard(map);
        guard.mode = Access::Refused;
        const auto tree = enumerate(guard, {});
        check(tree.outcome == Outcome::Refused && tree.reason == "trigger armed" &&
                  tree.modules.empty(),
              "a guard refusal refuses the tree");
    }
    {
        // A refusal mid-walk (the trigger armed, a run started) ends the walk:
        // refused, nothing partial reported as current.
        TestGuard guard(map);
        guard.refuseAfter = 5;
        const auto tree = enumerate(guard, {});
        check(tree.outcome == Outcome::Refused && tree.reason == "trigger armed" &&
                  tree.modules.empty() && guard.steps == 5,
              "a refusal mid-walk ends it with nothing partial");
    }
    {
        PlainPort plain;
        TestGuard guard(plain);
        const auto tree = enumerate(guard, {});
        check(tree.outcome == Outcome::Unsupported && tree.modules.empty(),
              "a port without introspection is unsupported");
    }
}

void enumerationRecordsSdkFailuresAndLimits() {
    FakeNodeMap map;
    std::mt19937 rng(7);
    const auto e = populate(map, Module::Remote, rng, 5, 30);
    FaultyPort faulty(map);
    faulty.failFeaturesOf.insert(e.categoryOrder[1]);
    faulty.failDescribe.insert(e.featureOrder[0]);
    TestGuard guard(faulty);
    TreeOptions options;
    options.modules = {Module::Remote, Module::Device};
    auto tree = enumerate(guard, options);
    check(tree.outcome == Outcome::Read, "SDK failures stay per node");
    check(!tree.modules[0].categories[1].error.empty(), "category failure recorded");
    check(!tree.modules[0].features[0].error.empty() && !tree.modules[0].features[0].descriptor,
          "describe failure recorded");
    faulty.failCategories = true;
    tree = enumerate(guard, options);
    check(tree.outcome == Outcome::Read && !tree.modules[0].enumerated &&
              !tree.modules[0].reason.empty(),
          "a module that cannot be enumerated says why");

    faulty.failCategories = false;
    options.limits.maxFeaturesPerModule = 5;
    options.limits.maxCategoriesPerModule = 2;
    tree = enumerate(guard, options);
    check(tree.modules[0].truncated && tree.modules[0].features.size() == 5 &&
              tree.modules[0].categories.size() == 2,
          "limits truncate and say so");
}

FakeNode numeric(const std::string& n, double value) {
    FakeNode node;
    node.name = n;
    node.kind = Kind::Float;
    node.value = value;
    node.minimum = 0.0;
    node.maximum = 100.0;
    return node;
}

void guardedWritesReadBackAndAdvanceTheRevision() {
    std::mt19937 rng(3);
    const auto gateName = name(rng, "G", 0), on = name(rng, "V", 0), off = name(rng, "V", 1);
    const auto targetName = name(rng, "T", 0), roName = name(rng, "R", 0);
    const auto selName = name(rng, "S", 0), selectedName = name(rng, "X", 0);
    FakeNodeMap map;
    FakeNode gate;
    gate.name = gateName;
    gate.kind = Kind::Enumeration;
    gate.entries = {on, off};
    gate.value = on;
    map.addFeature(Module::Remote, gate);
    auto target = numeric(targetName, 10.0);
    target.writableWhen = FakeNode::Condition{gateName, on};
    map.addFeature(Module::Remote, target);
    auto ro = numeric(roName, 1.0);
    ro.writable = false;
    map.addFeature(Module::Remote, ro);
    FakeNode sel;
    sel.name = selName;
    sel.kind = Kind::Enumeration;
    sel.entries = {on, off};
    sel.value = on;
    map.addFeature(Module::Remote, sel);
    FakeNode selected;
    selected.name = selectedName;
    selected.kind = Kind::Integer;
    selected.minimum = std::int64_t{0};
    selected.maximum = std::int64_t{9};
    selected.selector = selName;
    selected.selected = {{on, std::int64_t{1}}, {off, std::int64_t{2}}};
    map.addFeature(Module::Remote, selected);

    TestGuard guard(map);
    const auto token0 = guard.current.token;
    Request req{token0, {Module::Remote, targetName, std::nullopt}};

    auto r = guardedRead(guard, req);
    check(r.outcome == Outcome::Read && r.readback->writable, "read before write");

    r = guardedWrite(guard, req, Value{42.5});
    check(r.outcome == Outcome::ReadBack && std::get<double>(*r.readback->value) == 42.5 &&
              std::get<double>(*r.requested) == 42.5,
          "write reads back");
    check(r.session.capabilityRevision == token0.capabilityRevision + 1 &&
              guard.current.token.capabilityRevision == r.session.capabilityRevision,
          "a write advances the capability revision and returns the new token");
    const auto token1 = r.session;

    const auto writes = map.writes();
    r = guardedWrite(guard, req, Value{1.0}); // token0 is now stale
    check(r.outcome == Outcome::Stale && map.writes() == writes, "old revision refused");

    // A write to the gate changes the target's access; it is re-read, not cached.
    r = guardedWrite(guard, Request{token1, {Module::Remote, gateName, std::nullopt}},
                     Value{off});
    check(r.outcome == Outcome::ReadBack, "gate written");
    const auto token2 = r.session;
    r = guardedWrite(guard, Request{token2, {Module::Remote, targetName, std::nullopt}},
                     Value{5.0});
    check(r.outcome == Outcome::Refused && !r.readback->writable && map.writes() == writes + 1,
          "access re-read after a dependent write: refused without touching the port");
    check(r.session == token2, "a refusal does not advance the revision");

    r = guardedWrite(guard, Request{token2, {Module::Remote, roName, std::nullopt}}, Value{2.0});
    check(r.outcome == Outcome::Refused && r.readback && !r.readback->writable,
          "read-only node refused");
    r = guardedWrite(guard, Request{token2, {Module::Remote, name(rng, "N", 0), std::nullopt}},
                     Value{2.0});
    check(r.outcome == Outcome::Unsupported, "absent node unsupported");
    r = guardedWrite(guard, Request{token2, {Module::Remote, roName, std::nullopt}},
                     Value{std::int64_t{2}});
    check(r.outcome == Outcome::Refused, "read-only wins before a type check");

    // Selector-scoped write: that slot changes, the selector does not move.
    r = guardedWrite(guard, Request{token2, {Module::Remote, selectedName, off}},
                     Value{std::int64_t{7}});
    check(r.outcome == Outcome::ReadBack && std::get<std::int64_t>(*r.readback->value) == 7,
          "selected slot written");
    const auto token3 = r.session;
    r = guardedRead(guard, Request{token3, {Module::Remote, selectedName, std::nullopt}});
    check(std::get<std::int64_t>(*r.readback->value) == 1, "selector not moved by a scoped write");
    r = guardedRead(guard, Request{token3, {Module::Remote, selName, std::nullopt}});
    check(std::get<std::string>(*r.readback->value) == on, "selector value restored");

    // An exception after the write reached the node map is an unknown outcome.
    map.failNextWriteAfterApply("transport lost after submit");
    r = guardedWrite(guard, Request{token3, {Module::Remote, selectedName, on}},
                     Value{std::int64_t{3}});
    check(r.outcome == Outcome::UnknownWriteOutcome && !r.readback &&
              r.session.capabilityRevision == token3.capabilityRevision + 1,
          "unknown outcome is distinct and still invalidates cached views");

    // No session / no node map.
    guard.mode = Access::NoSession;
    r = guardedWrite(guard, Request{r.session, {Module::Remote, targetName, std::nullopt}},
                     Value{1.0});
    check(r.outcome == Outcome::Stale && r.requested, "closed session: stale, request echoed");
    guard.mode = Access::Unsupported;
    r = guardedRead(guard, Request{token3, {Module::Remote, targetName, std::nullopt}});
    check(r.outcome == Outcome::Unsupported, "no GenApi: unsupported");

    // A new generation (restart) refuses the old token.
    guard.mode = Access::Ran;
    ++guard.current.token.generation;
    r = guardedRead(guard, Request{token3, {Module::Remote, targetName, std::nullopt}});
    check(r.outcome == Outcome::Stale, "older generation refused");
}
} // namespace

// Guard refusals of single operations: refused with the guard's reason, the
// port untouched, the revision unchanged.
void guardedOperationsRefusedBeforeThePort() {
    FakeNodeMap map;
    std::mt19937 rng(9);
    const auto e = populate(map, Module::Remote, rng, 2, 4);
    TestGuard guard(map);
    guard.mode = Access::Refused;
    Request request;
    request.expected = guard.current.token;
    request.address = {Module::Remote, e.featureOrder[0], std::nullopt};
    const auto read = guardedRead(guard, request);
    check(read.outcome == Outcome::Refused && read.reason == "trigger armed" && !read.readback,
          "a refused read reaches nothing");
    const auto written = guardedWrite(guard, request, Value{1.0});
    check(written.outcome == Outcome::Refused && written.reason == "trigger armed" &&
              written.requested == Value{1.0} && map.writes() == 0,
          "a refused write reaches nothing and keeps the request apart");
    check(guard.current.token.capabilityRevision == 1, "a refusal does not advance the revision");
}

// ModuleWriteTracker: only a write to the watched module, or a selector-scoped
// describe there, counts as an operation that could move its selectors.
void moduleWriteTrackerCountsOnlySelectorMovers() {
    std::mt19937 rng(17);
    const auto iface = name(rng, "I", 0), remote = name(rng, "R", 0);
    FakeNodeMap map;
    map.addFeature(Module::Interface, numeric(iface, 1.0));
    map.addFeature(Module::Remote, numeric(remote, 1.0));
    {
        ModuleWriteTracker tracked(map, Module::Interface);
        tracked.describe({Module::Interface, iface, std::nullopt});
        tracked.categories(Module::Interface, false);
        tracked.features(Module::Interface, false);
        tracked.describe({Module::Remote, remote, std::nullopt});
        tracked.write({Module::Remote, remote, std::nullopt}, Value{2.0});
        check(!tracked.touched(), "reads and other modules leave the watched selectors alone");
        check(map.writes() == 1, "the decorator forwards writes");
    }
    {
        ModuleWriteTracker tracked(map, Module::Interface);
        tracked.write({Module::Interface, iface, std::nullopt}, Value{3.0});
        check(tracked.touched(), "a write to the watched module counts");
    }
    {
        ModuleWriteTracker tracked(map, Module::Interface);
        try {
            tracked.describe({Module::Interface, iface, std::string("Sel0")});
        } catch (const std::exception&) {
            // The fixture node has no selector; the attempt still counts.
        }
        check(tracked.touched(), "a selector-scoped describe of the watched module counts");
    }
    {
        ModuleWriteTracker tracked(map, Module::Interface);
        try {
            tracked.write({Module::Interface, "NoSuchNode", std::nullopt}, Value{1.0});
        } catch (const std::exception&) {
        }
        check(tracked.touched(), "a write that threw still counts (it may have been submitted)");
    }
}

int main() {
    enumerationMatchesTheNodeMapExactly();
    enumerationRefusesStaleAndUnsupported();
    enumerationRecordsSdkFailuresAndLimits();
    guardedWritesReadBackAndAdvanceTheRevision();
    guardedOperationsRefusedBeforeThePort();
    moduleWriteTrackerCountsOnlySelectorMovers();
    std::cout << "feature tree checks passed: " << checks << '\n';
    return 0;
}
