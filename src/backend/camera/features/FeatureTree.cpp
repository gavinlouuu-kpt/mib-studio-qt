#include "backend/camera/features/FeatureTree.h"

#include <algorithm>
#include <exception>
#include <set>
#include <unordered_set>

namespace camera::features {

namespace {

// One guarded step. The first step pins the session; every later step must see
// the same token or the walk ends as Stale.
class Walker {
public:
    Walker(GuardedPort& guarded, TreeResult& result) : guarded_(guarded), result_(result) {}

    template <class Body> bool step(Body&& body) {
        if (failed_) return false;
        bool stale = false;
        std::string reason;
        const auto access = guarded_.run(
            [&](Port& port, CurrentSession& current) {
                if (!pinned_) {
                    pinned_ = true;
                    result_.session = current.token;
                    result_.evidence = current.evidence;
                } else if (!(current.token == result_.session)) {
                    stale = true;
                    return;
                }
                body(port);
            },
            reason);
        if (access == Access::Unsupported) return fail(Outcome::Unsupported, reason);
        if (access == Access::NoSession)
            return fail(pinned_ ? Outcome::Stale : Outcome::Refused,
                        pinned_ ? "The camera session closed during enumeration: " + reason
                                : reason);
        if (stale)
            return fail(Outcome::Stale,
                        "Source, session or capability revision changed during enumeration; "
                        "enumerate again.");
        return true;
    }
    bool fail(Outcome outcome, std::string reason) {
        failed_ = true;
        result_.outcome = outcome;
        result_.reason = std::move(reason);
        return false;
    }
    bool failed() const { return failed_; }

private:
    GuardedPort& guarded_;
    TreeResult& result_;
    bool pinned_{false};
    bool failed_{false};
};

void appendUnique(std::vector<std::string>& out, std::unordered_set<std::string>& seen,
                  const std::vector<std::string>& names) {
    for (const auto& name : names)
        if (seen.insert(name).second) out.push_back(name);
}

// Returns false when the walk ended (Stale/Refused/Unsupported); module-level
// SDK failures are recorded on the module and the walk continues.
bool enumerateModule(Walker& walker, Module module, const TreeOptions& options,
                     ModuleTree& tree) {
    tree.module = module;
    std::vector<std::string> categoryNames;
    std::string moduleError;
    if (!walker.step([&](Port& port) {
            try {
                categoryNames = port.categories(module, options.availableOnly);
            } catch (const std::exception& e) {
                moduleError = e.what();
            }
        }))
        return false;
    if (!moduleError.empty()) {
        tree.reason = "Categories could not be enumerated: " + moduleError;
        return true;
    }
    {
        // The node map is authoritative, but a repeated name is one node.
        std::vector<std::string> unique;
        std::unordered_set<std::string> seen;
        appendUnique(unique, seen, categoryNames);
        categoryNames.swap(unique);
    }
    if (categoryNames.size() > options.limits.maxCategoriesPerModule) {
        categoryNames.resize(options.limits.maxCategoriesPerModule);
        tree.truncated = true;
    }
    const std::unordered_set<std::string> categorySet(categoryNames.begin(), categoryNames.end());

    std::vector<std::string> featureOrder;
    std::unordered_set<std::string> featureSeen;
    std::unordered_set<std::string> isChild;
    for (const auto& name : categoryNames) {
        CategoryEntry entry;
        entry.name = name;
        if (!walker.step([&](Port& port) {
                try {
                    entry.info = port.info(module, name);
                    entry.categories = port.categoriesOf(module, name, options.availableOnly);
                    auto direct = port.featuresOf(module, name, options.availableOnly);
                    // A GenApi category lists its child categories as features too;
                    // keep each node in exactly one role.
                    direct.erase(std::remove_if(direct.begin(), direct.end(),
                                                [&](const std::string& n) {
                                                    return categorySet.count(n) != 0;
                                                }),
                                 direct.end());
                    entry.features = std::move(direct);
                } catch (const std::exception& e) {
                    entry.error = e.what();
                }
            }))
            return false;
        for (const auto& child : entry.categories) isChild.insert(child);
        appendUnique(featureOrder, featureSeen, entry.features);
        tree.categories.push_back(std::move(entry));
    }
    for (const auto& name : categoryNames)
        if (!isChild.count(name)) tree.roots.push_back(name);

    std::vector<std::string> all;
    if (!walker.step([&](Port& port) {
            try {
                all = port.features(module, options.availableOnly);
            } catch (const std::exception& e) {
                moduleError = e.what();
            }
        }))
        return false;
    if (!moduleError.empty()) {
        tree.reason = "Features could not be enumerated: " + moduleError;
        return true;
    }
    for (const auto& name : all) {
        if (categorySet.count(name)) continue;
        if (featureSeen.insert(name).second) {
            featureOrder.push_back(name);
            tree.uncategorized.push_back(name);
        }
    }
    if (featureOrder.size() > options.limits.maxFeaturesPerModule) {
        featureOrder.resize(options.limits.maxFeaturesPerModule);
        tree.truncated = true;
        const std::unordered_set<std::string> kept(featureOrder.begin(), featureOrder.end());
        tree.uncategorized.erase(std::remove_if(tree.uncategorized.begin(),
                                                tree.uncategorized.end(),
                                                [&](const std::string& n) {
                                                    return kept.count(n) == 0;
                                                }),
                                 tree.uncategorized.end());
    }

    for (const auto& name : featureOrder) {
        FeatureEntry entry;
        entry.name = name;
        if (!walker.step([&](Port& port) {
                try {
                    entry.info = port.info(module, name);
                    if (!options.descriptors) return;
                    const Address address{module, name, std::nullopt};
                    if (const auto invalid = validateAddress(address)) {
                        entry.error = "Not addressable by typed control: " + *invalid;
                        return;
                    }
                    entry.descriptor = port.describe(address);
                } catch (const std::exception& e) {
                    entry.error = e.what();
                }
            }))
            return false;
        tree.features.push_back(std::move(entry));
    }
    tree.enumerated = true;
    return true;
}

bool reachedDevice(Outcome outcome) {
    return outcome == Outcome::ReadBack || outcome == Outcome::WrittenUnconfirmed ||
           outcome == Outcome::UnknownWriteOutcome;
}

Result noSessionResult(Access access, const Request& request, const std::string& reason) {
    Result r;
    r.address = request.address;
    r.outcome = access == Access::Unsupported ? Outcome::Unsupported : Outcome::Stale;
    r.reason = access == Access::Unsupported
                   ? reason
                   : "No open camera session matches the request: " + reason;
    return r;
}

} // namespace

TreeResult enumerate(GuardedPort& guarded, const TreeOptions& options) {
    TreeResult result;
    Walker walker(guarded, result);
    bool enumerable = false;
    if (!walker.step([&](Port& port) { enumerable = port.canEnumerate(); })) return result;
    if (!enumerable) {
        result.outcome = Outcome::Unsupported;
        result.reason = "The camera's port does not expose node-map introspection.";
        return result;
    }
    std::set<Module> done;
    for (const auto module : options.modules) {
        if (!done.insert(module).second) continue;
        ModuleTree tree;
        if (!enumerateModule(walker, module, options, tree)) {
            result.modules.clear(); // nothing partial is reported as current
            return result;
        }
        result.modules.push_back(std::move(tree));
    }
    result.outcome = Outcome::Read;
    return result;
}

Result guardedRead(GuardedPort& guarded, const Request& request) {
    Result r;
    std::string reason;
    const auto access = guarded.run(
        [&](Port& port, CurrentSession& current) { r = read(port, current, request); }, reason);
    if (access != Access::Ran) return noSessionResult(access, request, reason);
    return r;
}

Result guardedWrite(GuardedPort& guarded, const Request& request, const Value& value) {
    Result r;
    std::string reason;
    const auto access = guarded.run(
        [&](Port& port, CurrentSession& current) {
            r = write(port, current, request, value);
            if (reachedDevice(r.outcome)) {
                ++current.token.capabilityRevision;
                r.session = current.token;
            }
        },
        reason);
    if (access != Access::Ran) {
        auto refused = noSessionResult(access, request, reason);
        refused.requested = value;
        return refused;
    }
    return r;
}

const char* toString(Module module) {
    switch (module) {
    case Module::System: return "system";
    case Module::Interface: return "interface";
    case Module::Device: return "device";
    case Module::Stream: return "stream";
    case Module::Remote: return "remote";
    }
    return "unknown";
}

const char* toString(Kind kind) {
    switch (kind) {
    case Kind::Integer: return "integer";
    case Kind::Float: return "float";
    case Kind::Boolean: return "boolean";
    case Kind::String: return "string";
    case Kind::Enumeration: return "enumeration";
    case Kind::Command: return "command";
    case Kind::Unsupported: return "unsupported";
    }
    return "unsupported";
}

const char* toString(Outcome outcome) {
    switch (outcome) {
    case Outcome::Read: return "read";
    case Outcome::ReadBack: return "readback";
    case Outcome::WrittenUnconfirmed: return "written_unconfirmed";
    case Outcome::Refused: return "refused";
    case Outcome::Stale: return "stale";
    case Outcome::Unsupported: return "unsupported";
    case Outcome::Error: return "error";
    case Outcome::UnknownWriteOutcome: return "unknown_write_outcome";
    }
    return "error";
}

std::optional<Module> moduleFromString(const std::string& text) {
    for (const auto m : {Module::System, Module::Interface, Module::Device, Module::Stream,
                         Module::Remote})
        if (text == toString(m)) return m;
    return std::nullopt;
}

} // namespace camera::features
