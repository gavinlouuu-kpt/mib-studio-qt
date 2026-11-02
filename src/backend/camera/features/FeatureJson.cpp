#include "backend/camera/features/FeatureJson.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <set>
#include <stdexcept>

namespace camera::features {

namespace {
using nlohmann::json;

std::string dump(const json& j) {
    // Device strings are not guaranteed UTF-8; never throw on them.
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

json valueJson(const std::optional<Value>& value) {
    if (!value) return nullptr;
    return std::visit(
        [](const auto& v) -> json {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::int64_t>)
                return {{"type", "integer"}, {"value", std::to_string(v)}}; // lossless
            else if constexpr (std::is_same_v<T, double>)
                return {{"type", "float"}, {"value", std::isfinite(v) ? json(v) : json(nullptr)}};
            else if constexpr (std::is_same_v<T, bool>)
                return {{"type", "boolean"}, {"value", v}};
            else
                return {{"type", "string"}, {"value", v}};
        },
        *value);
}

json tokenJson(const SessionToken& token) {
    if (token.source.empty() && token.generation == 0) return nullptr;
    return {{"instance", token.instance},
            {"source", token.source},
            {"generation", std::to_string(token.generation)},
            {"capability_revision", std::to_string(token.capabilityRevision)}};
}

json infoFields(json j, const NodeInfo& info) {
    j["display_name"] = info.displayName;
    j["tooltip"] = info.toolTip;
    j["description"] = info.description;
    j["visibility"] = info.visibility;
    return j;
}

json descriptorJson(const std::optional<Descriptor>& d) {
    if (!d) return nullptr;
    return {{"kind", toString(d->kind)},
            {"implemented", d->implemented},
            {"readable", d->readable},
            {"writable", d->writable},
            {"access_reason", d->accessReason},
            {"visibility", d->visibility},
            {"unit", d->unit},
            {"value", valueJson(d->value)},
            {"minimum", valueJson(d->minimum)},
            {"maximum", valueJson(d->maximum)},
            {"increment", valueJson(d->increment)},
            {"entries", d->entries}};
}

json addressJson(const Address& a) {
    return {{"module", toString(a.module)},
            {"feature", a.feature},
            {"selector", a.selectorValue ? json(*a.selectorValue) : json(nullptr)}};
}

json resultJson(const Result& r) {
    return {{"schema_version", kFeatureSchemaVersion},
            {"outcome", toString(r.outcome)},
            {"reason", r.reason},
            {"evidence", r.evidence},
            {"session", tokenJson(r.session)},
            {"address", addressJson(r.address)},
            {"requested", valueJson(r.requested)},
            {"readback", descriptorJson(r.readback)}};
}

json parseObject(const std::string& text) {
    if (text.size() > kFeatureRequestMaxBytes)
        throw std::invalid_argument("Request exceeds " + std::to_string(kFeatureRequestMaxBytes) +
                                    " bytes.");
    auto j = json::parse(text.empty() ? std::string("{}") : text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) throw std::invalid_argument("Request is not a JSON object.");
    return j;
}

void onlyKeys(const json& j, std::initializer_list<const char*> allowed, const char* what) {
    const std::set<std::string> keys(allowed.begin(), allowed.end());
    for (const auto& item : j.items())
        if (!keys.count(item.key()))
            throw std::invalid_argument(std::string("Unknown ") + what + " key: " + item.key());
}

std::uint64_t decimal(const json& j, const char* name) {
    if (!j.is_string()) throw std::invalid_argument(std::string(name) + " must be a decimal string.");
    const auto text = j.get<std::string>();
    if (text.empty() || text.size() > 20 || text.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument(std::string(name) + " must be a decimal string.");
    try {
        return std::stoull(text);
    } catch (const std::exception&) {
        throw std::invalid_argument(std::string(name) + " is out of range.");
    }
}

std::string text(const json& j, const char* name) {
    if (!j.is_string()) throw std::invalid_argument(std::string(name) + " must be a string.");
    return j.get<std::string>();
}

Request parseRequest(const json& j) {
    Request request;
    if (!j.contains("session") || !j.at("session").is_object())
        throw std::invalid_argument(
            "session {instance, source, generation, capability_revision} is required.");
    const auto& s = j.at("session");
    onlyKeys(s, {"instance", "source", "generation", "capability_revision"}, "session");
    if (!s.contains("instance") || !s.contains("source") || !s.contains("generation") ||
        !s.contains("capability_revision"))
        throw std::invalid_argument(
            "session needs instance, source, generation and capability_revision.");
    request.expected.instance = text(s.at("instance"), "session.instance");
    request.expected.source = text(s.at("source"), "session.source");
    request.expected.generation = decimal(s.at("generation"), "session.generation");
    request.expected.capabilityRevision =
        decimal(s.at("capability_revision"), "session.capability_revision");
    if (!j.contains("module")) throw std::invalid_argument("module is required.");
    const auto module = moduleFromString(text(j.at("module"), "module"));
    if (!module) throw std::invalid_argument("module must be system, interface, device, stream or remote.");
    request.address.module = *module;
    if (!j.contains("feature")) throw std::invalid_argument("feature is required.");
    request.address.feature = text(j.at("feature"), "feature");
    if (j.contains("selector") && !j.at("selector").is_null())
        request.address.selectorValue = text(j.at("selector"), "selector");
    return request;
}

Value parseValue(const json& j) {
    if (!j.is_object()) throw std::invalid_argument("value must be {type, value}.");
    onlyKeys(j, {"type", "value"}, "value");
    if (!j.contains("type") || !j.contains("value"))
        throw std::invalid_argument("value must be {type, value}.");
    const auto type = text(j.at("type"), "value.type");
    const auto& v = j.at("value");
    if (type == "integer") {
        // Decimal string, lossless above 2^53; a leading '-' allowed.
        const auto t = text(v, "value.value");
        const auto digits = t.size() > 1 && t.front() == '-' ? t.substr(1) : t;
        if (digits.empty() || t.size() > 20 || digits.find_first_not_of("0123456789") != std::string::npos)
            throw std::invalid_argument("An integer value is a decimal string.");
        try {
            return Value{static_cast<std::int64_t>(std::stoll(t))};
        } catch (const std::exception&) {
            throw std::invalid_argument("Integer value is out of range.");
        }
    }
    if (type == "float") {
        if (!v.is_number()) throw std::invalid_argument("A float value is a JSON number.");
        return Value{v.get<double>()};
    }
    if (type == "boolean") {
        if (!v.is_boolean()) throw std::invalid_argument("A boolean value is true or false.");
        return Value{v.get<bool>()};
    }
    if (type == "string") return Value{text(v, "value.value")};
    throw std::invalid_argument("value.type must be integer, float, boolean or string.");
}

json moduleJson(const ModuleTree& m) {
    json categories = json::array();
    for (const auto& c : m.categories)
        categories.push_back(infoFields({{"name", c.name},
                                         {"categories", c.categories},
                                         {"features", c.features},
                                         {"error", c.error}},
                                        c.info));
    json features = json::array();
    for (const auto& f : m.features)
        features.push_back(infoFields(
            {{"name", f.name}, {"descriptor", descriptorJson(f.descriptor)}, {"error", f.error}},
            f.info));
    return {{"module", toString(m.module)}, {"enumerated", m.enumerated},
            {"reason", m.reason},           {"truncated", m.truncated},
            {"roots", m.roots},             {"categories", categories},
            {"features", features},         {"uncategorized", m.uncategorized}};
}

json treeRefused(const std::string& reason) {
    return {{"schema_version", kFeatureSchemaVersion},
            {"outcome", toString(Outcome::Refused)},
            {"reason", reason},
            {"evidence", ""},
            {"session", nullptr},
            {"modules", json::array()}};
}

} // namespace

std::string featureRefusedJson(const std::string& reason) {
    Result r;
    r.outcome = Outcome::Refused;
    r.reason = reason;
    return dump(resultJson(r));
}

std::string featureTreeJson(GuardedPort& guarded, const std::string& requestJson) {
    TreeOptions options;
    try {
        const auto j = parseObject(requestJson);
        onlyKeys(j, {"modules", "descriptors", "available_only"}, "request");
        if (j.contains("modules") && !j.at("modules").is_null()) {
            if (!j.at("modules").is_array() || j.at("modules").empty() || j.at("modules").size() > 5)
                throw std::invalid_argument("modules must be a list of 1..5 module names.");
            options.modules.clear();
            for (const auto& name : j.at("modules")) {
                const auto module = moduleFromString(text(name, "modules[]"));
                if (!module) throw std::invalid_argument("Unknown module in modules.");
                options.modules.push_back(*module);
            }
        }
        if (j.contains("descriptors")) {
            if (!j.at("descriptors").is_boolean()) throw std::invalid_argument("descriptors is a boolean.");
            options.descriptors = j.at("descriptors").get<bool>();
        }
        if (j.contains("available_only")) {
            if (!j.at("available_only").is_boolean())
                throw std::invalid_argument("available_only is a boolean.");
            options.availableOnly = j.at("available_only").get<bool>();
        }
    } catch (const std::exception& e) {
        return dump(treeRefused(e.what()));
    }
    const auto tree = enumerate(guarded, options);
    json modules = json::array();
    for (const auto& m : tree.modules) modules.push_back(moduleJson(m));
    return dump({{"schema_version", kFeatureSchemaVersion},
                 {"outcome", toString(tree.outcome)},
                 {"reason", tree.reason},
                 {"evidence", tree.evidence},
                 {"session", tokenJson(tree.session)},
                 {"modules", modules}});
}

std::string featureReadJson(GuardedPort& guarded, const std::string& requestJson) {
    Request request;
    try {
        const auto j = parseObject(requestJson);
        onlyKeys(j, {"session", "module", "feature", "selector"}, "request");
        request = parseRequest(j);
    } catch (const std::exception& e) {
        return featureRefusedJson(e.what());
    }
    return dump(resultJson(guardedRead(guarded, request)));
}

std::string featureWriteJson(GuardedPort& guarded, const std::string& requestJson) {
    Request request;
    Value value;
    try {
        const auto j = parseObject(requestJson);
        onlyKeys(j, {"session", "module", "feature", "selector", "value"}, "request");
        request = parseRequest(j);
        if (!j.contains("value")) throw std::invalid_argument("value {type, value} is required.");
        value = parseValue(j.at("value"));
    } catch (const std::exception& e) {
        return featureRefusedJson(e.what());
    }
    return dump(resultJson(guardedWrite(guarded, request, value)));
}

} // namespace camera::features
