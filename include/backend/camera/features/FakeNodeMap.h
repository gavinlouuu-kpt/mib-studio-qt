#pragma once

#include "backend/camera/features/FeatureControl.h"

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

// A data-driven, in-memory GenApi node map behind the same Port interface as
// the eGrabber adapter. Its owner decides its contents; it carries no feature
// list of its own. Evidence class: "fixture/mock", never "device".
//
// Semantics mirrored from GenApi so callers exercise the real rules:
// - access is evaluated on every describe (a node may be writable only while
//   another node holds a value), so re-reading after a write sees the change;
// - a selected node keeps one value per selector entry; a selector-scoped
//   address reads/writes that slot without moving the selector (rollback);
// - write re-checks access, type and bounds itself (the SDK's final authority).
// Not thread-safe: the owner serializes access under its session guard.
namespace camera::features {

struct FakeNode {
    std::string name;
    Kind kind{Kind::Integer};
    NodeInfo info;
    bool implemented{true};
    bool readable{true};
    bool writable{true};
    std::string unit;
    std::optional<Value> value;
    std::optional<Value> minimum;
    std::optional<Value> maximum;
    std::optional<Value> increment;
    std::vector<std::string> entries;
    // Writable only while `feature` (same module) currently equals `equals`.
    struct Condition {
        std::string feature;
        Value equals;
    };
    std::optional<Condition> writableWhen;
    // Selected node: `selector` names an Enumeration node of the same module;
    // `selected` holds the value for each of its entries.
    std::string selector;
    std::map<std::string, Value> selected;
};

class FakeNodeMap final : public Port {
public:
    // `parent` empty = a root category.
    void addCategory(Module module, const std::string& name, NodeInfo info,
                     const std::string& parent = {});
    // `category` empty = outside every category.
    void addFeature(Module module, FakeNode node, const std::string& category = {});

    // Port
    Descriptor describe(const Address& address) override;
    void write(const Address& address, const Value& value) override;
    bool canEnumerate() const override { return true; }
    std::vector<std::string> categories(Module module, bool availableOnly) override;
    std::vector<std::string> categoriesOf(Module module, const std::string& category,
                                          bool availableOnly) override;
    std::vector<std::string> featuresOf(Module module, const std::string& category,
                                        bool availableOnly) override;
    std::vector<std::string> features(Module module, bool availableOnly) override;
    NodeInfo info(Module module, const std::string& node) override;

    // Fixture controls/observations (owner only).
    std::size_t writes() const { return writes_; }
    // The next write throws after it has been applied (an unknown outcome).
    void failNextWriteAfterApply(std::string message) { failAfterApply_ = std::move(message); }

private:
    struct Category {
        NodeInfo info;
        std::string parent;
        std::vector<std::string> categories;
        std::vector<std::string> features;
    };
    struct ModuleMap {
        std::vector<std::string> categoryOrder;
        std::map<std::string, Category> categories;
        std::vector<std::string> featureOrder;
        std::map<std::string, FakeNode> nodes;
    };
    const FakeNode* find(Module module, const std::string& name) const;
    FakeNode& node(Module module, const std::string& name);
    bool writableNow(Module module, const FakeNode& node) const;
    bool available(Module module, const FakeNode& node) const;
    std::string selectorSlot(Module module, const FakeNode& node, const Address& address) const;

    std::map<Module, ModuleMap> modules_;
    std::size_t writes_{0};
    std::optional<std::string> failAfterApply_;
};

} // namespace camera::features
