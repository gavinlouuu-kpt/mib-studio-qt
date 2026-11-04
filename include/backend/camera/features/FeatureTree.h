#pragma once

#include "backend/camera/features/FeatureControl.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// E02 (E-1/E-2): node-map enumeration and guarded typed access. SDK-free and
// Qt-free; the device facts come only from a Port, the session only from a
// GuardedPort. Nothing here names a production feature.
namespace camera::features {

// Service-side access to the current camera session's port. The implementation
// (CaptureService) holds its session guard for the whole call, builds the
// CurrentSession (token = source + capture generation + capability revision)
// and stores back any revision `fn` advances. Refused: the guard declined the
// operation before touching the device (sort pulses pending, a run is in
// progress) and `reason` says why; replies carry outcome "refused".
enum class Access { Ran, NoSession, Unsupported, Refused };
class GuardedPort {
public:
    virtual ~GuardedPort() = default;
    virtual Access run(const std::function<void(Port&, CurrentSession&)>& fn,
                       std::string& reason) = 0;
};

struct TreeLimits {
    std::size_t maxCategoriesPerModule{2048};
    std::size_t maxFeaturesPerModule{8192};
};

struct TreeOptions {
    std::vector<Module> modules{Module::System, Module::Interface, Module::Device, Module::Stream,
                                Module::Remote};
    bool availableOnly{false};
    bool descriptors{true}; // describe() every feature (type, access, value, bounds, entries)
    TreeLimits limits{};
};

struct CategoryEntry {
    std::string name;
    NodeInfo info;
    std::vector<std::string> categories; // direct child categories, node-map order
    std::vector<std::string> features;   // direct features, node-map order
    std::string error;                   // non-empty when this category could not be read
};

struct FeatureEntry {
    std::string name;
    NodeInfo info;
    std::optional<Descriptor> descriptor;
    std::string error; // non-empty when the feature could not be described
};

struct ModuleTree {
    Module module{Module::Remote};
    bool enumerated{false};
    std::string reason; // why the module could not be enumerated
    bool truncated{false};
    std::vector<std::string> roots; // categories that are no other category's child
    std::vector<CategoryEntry> categories;
    std::vector<FeatureEntry> features;
    std::vector<std::string> uncategorized; // features outside every category
};

struct TreeResult {
    Outcome outcome{Outcome::Error}; // Read, Stale, Refused, Unsupported or Error
    SessionToken session;
    std::string evidence;
    std::string reason;
    std::vector<ModuleTree> modules;
};

// Enumerates the node map one node per guarded step, so the session guard is
// never held across the whole tree. Every step re-checks the token pinned by
// the first step; a change (stop, restart, a write elsewhere) ends the walk as
// Stale and returns nothing partial as current.
TreeResult enumerate(GuardedPort& guarded, const TreeOptions& options);

// read()/write() of FeatureControl under the guard. A write that may have
// reached the device (ReadBack, WrittenUnconfirmed, UnknownWriteOutcome)
// advances the capability revision: the node map may have changed (selectors,
// dependent access), so views built under the old revision are refused as
// Stale. The reply carries the advanced token.
Result guardedRead(GuardedPort& guarded, const Request& request);
Result guardedWrite(GuardedPort& guarded, const Request& request, const Value& value);

const char* toString(Module module);
const char* toString(Kind kind);
const char* toString(Outcome outcome);
std::optional<Module> moduleFromString(const std::string& text);

} // namespace camera::features
