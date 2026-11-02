#include "backend/camera/features/FakeNodeMap.h"
#include "backend/camera/features/FeatureTree.h"

#include <algorithm>
#include <stdexcept>

namespace camera::features {

namespace {
bool sameType(const Value& a, const Value& b) { return a.index() == b.index(); }
// A module the fixture does not define is absent, not an empty node map.
[[noreturn]] void absent(Module module) {
    throw ModuleAbsent(std::string("Module ") + toString(module) +
                       " is not present in this node map.");
}
} // namespace

void FakeNodeMap::addCategory(Module module, const std::string& name, NodeInfo info,
                              const std::string& parent) {
    auto& map = modules_[module];
    if (map.categories.count(name) || map.nodes.count(name))
        throw std::invalid_argument("Duplicate node: " + name);
    if (!parent.empty()) {
        auto it = map.categories.find(parent);
        if (it == map.categories.end()) throw std::invalid_argument("Unknown parent: " + parent);
        it->second.categories.push_back(name);
    }
    map.categoryOrder.push_back(name);
    map.categories[name] = Category{std::move(info), parent, {}, {}};
}

void FakeNodeMap::addFeature(Module module, FakeNode n, const std::string& category) {
    auto& map = modules_[module];
    if (map.categories.count(n.name) || map.nodes.count(n.name))
        throw std::invalid_argument("Duplicate node: " + n.name);
    if (!category.empty()) {
        auto it = map.categories.find(category);
        if (it == map.categories.end())
            throw std::invalid_argument("Unknown category: " + category);
        it->second.features.push_back(n.name);
    }
    map.featureOrder.push_back(n.name);
    const auto name = n.name;
    map.nodes.emplace(name, std::move(n));
}

const FakeNode* FakeNodeMap::find(Module module, const std::string& name) const {
    const auto m = modules_.find(module);
    if (m == modules_.end()) return nullptr;
    const auto it = m->second.nodes.find(name);
    return it == m->second.nodes.end() ? nullptr : &it->second;
}

FakeNode& FakeNodeMap::node(Module module, const std::string& name) {
    auto& nodes = modules_[module].nodes;
    const auto it = nodes.find(name);
    if (it == nodes.end()) throw std::runtime_error("Feature not found: " + name);
    return it->second;
}

bool FakeNodeMap::writableNow(Module module, const FakeNode& n) const {
    if (!n.implemented || !n.writable) return false;
    if (!n.writableWhen) return true;
    const auto* gate = find(module, n.writableWhen->feature);
    if (!gate || !gate->implemented) return false;
    const auto& v = gate->selector.empty() ? gate->value : std::optional<Value>{};
    return v && *v == n.writableWhen->equals;
}

bool FakeNodeMap::available(Module module, const FakeNode& n) const {
    return n.implemented && (n.readable || writableNow(module, n));
}

std::string FakeNodeMap::selectorSlot(Module module, const FakeNode& n,
                                      const Address& address) const {
    if (n.selector.empty()) {
        if (address.selectorValue)
            throw std::runtime_error("Feature is not selected by any selector: " + n.name);
        return {};
    }
    const auto* sel = find(module, n.selector);
    if (!sel || sel->kind != Kind::Enumeration || !sel->value ||
        !std::holds_alternative<std::string>(*sel->value))
        throw std::runtime_error("Selector of " + n.name + " is unavailable.");
    const auto slot = address.selectorValue ? *address.selectorValue
                                            : std::get<std::string>(*sel->value);
    if (std::find(sel->entries.begin(), sel->entries.end(), slot) == sel->entries.end())
        throw std::runtime_error("Selector entry is not available: " + slot);
    return slot;
}

Descriptor FakeNodeMap::describe(const Address& address) {
    if (const auto invalid = validateAddress(address)) throw std::invalid_argument(*invalid);
    Descriptor d;
    const auto* n = find(address.module, address.feature);
    if (!n || !n->implemented) {
        d.accessReason = "Feature is not implemented by this source/module.";
        return d;
    }
    const auto slot = selectorSlot(address.module, *n, address);
    d.kind = n->kind;
    d.implemented = true;
    d.readable = n->readable;
    d.writable = writableNow(address.module, *n);
    if (!d.writable)
        d.accessReason =
            d.readable ? "Read-only in the current context." : "Unavailable in the current context.";
    d.visibility = n->info.visibility;
    d.unit = n->unit;
    d.minimum = n->minimum;
    d.maximum = n->maximum;
    d.increment = n->increment;
    d.entries = n->entries;
    if (d.readable) {
        if (slot.empty()) {
            d.value = n->value;
        } else if (const auto it = n->selected.find(slot); it != n->selected.end()) {
            d.value = it->second;
        }
    }
    return d;
}

void FakeNodeMap::write(const Address& address, const Value& value) {
    if (const auto invalid = validateAddress(address)) throw std::invalid_argument(*invalid);
    auto& n = node(address.module, address.feature);
    if (!writableNow(address.module, n))
        throw std::runtime_error("Feature is not writable now: " + n.name);
    const auto slot = selectorSlot(address.module, n, address);
    Descriptor bounds;
    bounds.kind = n.kind;
    bounds.minimum = n.minimum;
    bounds.maximum = n.maximum;
    bounds.increment = n.increment;
    bounds.entries = n.entries;
    if (const auto invalid = validateValue(bounds, value)) throw std::runtime_error(*invalid);
    std::optional<Value> current = n.value;
    if (!slot.empty()) {
        const auto it = n.selected.find(slot);
        current = it == n.selected.end() ? std::nullopt : std::optional<Value>{it->second};
    }
    if (current && !sameType(*current, value))
        throw std::runtime_error("Value type does not match the node.");
    if (slot.empty())
        n.value = value;
    else
        n.selected[slot] = value;
    ++writes_;
    if (failAfterApply_) {
        auto message = std::move(*failAfterApply_);
        failAfterApply_.reset();
        throw std::runtime_error(message);
    }
}

std::vector<std::string> FakeNodeMap::categories(Module module, bool) {
    const auto m = modules_.find(module);
    if (m == modules_.end()) absent(module);
    return m->second.categoryOrder;
}

std::vector<std::string> FakeNodeMap::categoriesOf(Module module, const std::string& category,
                                                   bool) {
    const auto m = modules_.find(module);
    if (m == modules_.end() || !m->second.categories.count(category))
        throw std::runtime_error("Category not found: " + category);
    return m->second.categories.at(category).categories;
}

std::vector<std::string> FakeNodeMap::featuresOf(Module module, const std::string& category,
                                                 bool availableOnly) {
    const auto m = modules_.find(module);
    if (m == modules_.end() || !m->second.categories.count(category))
        throw std::runtime_error("Category not found: " + category);
    std::vector<std::string> out;
    for (const auto& name : m->second.categories.at(category).features)
        if (!availableOnly || available(module, m->second.nodes.at(name))) out.push_back(name);
    return out;
}

std::vector<std::string> FakeNodeMap::features(Module module, bool availableOnly) {
    const auto m = modules_.find(module);
    if (m == modules_.end()) absent(module);
    std::vector<std::string> out;
    for (const auto& name : m->second.featureOrder)
        if (!availableOnly || available(module, m->second.nodes.at(name))) out.push_back(name);
    return out;
}

NodeInfo FakeNodeMap::info(Module module, const std::string& name) {
    const auto m = modules_.find(module);
    if (m != modules_.end()) {
        if (const auto c = m->second.categories.find(name); c != m->second.categories.end())
            return c->second.info;
        if (const auto f = m->second.nodes.find(name); f != m->second.nodes.end())
            return f->second.info;
    }
    throw std::runtime_error("Node not found: " + name);
}

} // namespace camera::features
