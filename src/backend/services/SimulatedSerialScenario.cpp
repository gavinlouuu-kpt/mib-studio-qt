#include "backend/services/SimulatedSerialScenario.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace backend::services::serialsim {

namespace {

using json = nlohmann::json;

[[noreturn]] void fail(const std::string& where, const std::string& what)
{
    throw std::runtime_error("serial-sim scenario: " + where + ": " + what);
}

uint16_t hexId(const json& node, const char* field, const std::string& where)
{
    if (!node.contains(field)) return 0;
    const auto& v = node.at(field);
    if (v.is_number_unsigned()) return static_cast<uint16_t>(v.get<unsigned>());
    if (!v.is_string()) fail(where, std::string(field) + " must be a hex string like \"1a86\"");
    try {
        return static_cast<uint16_t>(std::stoul(v.get<std::string>(), nullptr, 16));
    } catch (const std::exception&) {
        fail(where, std::string(field) + " is not hex");
    }
}

uint8_t address(const json& device, const std::string& where)
{
    const int a = device.value("address", 1);
    if (a < 1 || a > 247) fail(where, "address must be 1..247");
    return static_cast<uint8_t>(a);
}

std::shared_ptr<SimDevice> makeDevice(const json& device, const std::string& where)
{
    if (!device.is_object() || !device.contains("type")) fail(where, "a device needs a \"type\"");
    const std::string type = device.at("type").get<std::string>();
    if (type == "oeabt") return std::make_shared<OeabtDevice>(device.value("baud", 115200));
    if (type == "pulse-generator") return makePulseGenerator(address(device, where), device.value("baud", 9600));
    if (type == "dlsp-pump") return makeDlspPump(address(device, where), device.value("baud", 115200));
    if (type == "tushui-pump") return makeTushuiPump(address(device, where), device.value("baud", 115200));
    if (type == "zc300") return makeZc300(address(device, where), device.value("serial", 26017));
    if (type == "modbus") {
        auto generic = std::make_shared<ModbusRegisterDevice>(device.value("kind", std::string("modbus")),
                                                              address(device, where), device.value("baud", 9600));
        for (const char* bank : {"holding", "input"}) {
            if (!device.contains(bank)) continue;
            auto& regs = std::string(bank) == "holding" ? generic->holding : generic->input;
            for (const auto& [reg, value] : device.at(bank).items()) {
                regs[static_cast<uint16_t>(std::stoul(reg))] = value.get<uint16_t>();
            }
        }
        return generic;
    }
    fail(where, "unknown device type \"" + type +
                    "\" (oeabt, pulse-generator, dlsp-pump, tushui-pump, zc300, modbus)");
}

} // namespace

std::shared_ptr<Rig> loadScenario(const std::string& jsonText, std::optional<uint64_t> seedOverride)
{
    json doc;
    try {
        doc = json::parse(jsonText);
    } catch (const json::exception& e) {
        fail("parse", e.what());
    }
    if (!doc.is_object() || !doc.contains("ports") || !doc.at("ports").is_array()) {
        fail("root", "expected an object with a \"ports\" array");
    }
    try {
        auto rig = std::make_shared<Rig>(seedOverride.value_or(doc.value("seed", uint64_t{1})));
        rig->setExclusiveOpen(doc.value("exclusiveOpen", true));
        for (const auto& node : doc.at("ports")) {
            if (!node.contains("name")) fail("ports[]", "a port needs a \"name\"");
            PortSpec spec;
            spec.systemName = node.at("name").get<std::string>();
            const std::string where = "port " + spec.systemName;
            spec.description = node.value("description", spec.description);
            spec.manufacturer = node.value("manufacturer", spec.manufacturer);
            spec.serialNumber = node.value("serial", std::string{});
            spec.vendorId = hexId(node, "vid", where);
            spec.productId = hexId(node, "pid", where);
            spec.presentAfterMs = node.value("presentAfterMs", 0);
            spec.openLatencyMs = node.value("openLatencyMs", 0);
            if (node.contains("replyLatencyMs")) {
                const auto& range = node.at("replyLatencyMs");
                if (!range.is_array() || range.size() != 2) fail(where, "replyLatencyMs must be [min, max]");
                spec.replyLatencyMinMs = range[0].get<int>();
                spec.replyLatencyMaxMs = range[1].get<int>();
            }
            spec.silentAfterOpenMs = node.value("silentAfterOpenMs", 0);
            spec.fragmentReplies = node.value("fragmentReplies", false);
            if (node.contains("unplugged")) {
                for (const auto& window : node.at("unplugged")) {
                    spec.unpluggedWindowsMs.emplace_back(window.at("fromMs").get<int>(),
                                                         window.value("toMs", -1));
                }
            }
            rig->addPort(spec);
            if (node.contains("devices")) {
                for (const auto& device : node.at("devices")) {
                    rig->addDevice(spec.systemName, makeDevice(device, where));
                }
            }
        }
        return rig;
    } catch (const json::exception& e) {
        fail("field", e.what());
    }
}

bool installFromEnvironment(std::string& error)
{
    error.clear();
    const char* mode = std::getenv("MIB_SERIAL_MODE");
    std::string value = mode ? mode : "";
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (value.empty() || value == "hardware") return false;
    if (value != "sim") {
        error = "MIB_SERIAL_MODE=" + value + " is not a serial mode (use sim or hardware)";
        return false;
    }

    std::optional<uint64_t> seed;
    if (const char* s = std::getenv("MIB_SERIAL_SIM_SEED"); s && *s) {
        try {
            seed = std::stoull(s);
        } catch (const std::exception&) {
            error = std::string("MIB_SERIAL_SIM_SEED=") + s + " is not a number";
            return false;
        }
    }

    std::shared_ptr<Rig> rig;
    const char* path = std::getenv("MIB_SERIAL_SIM_SCENARIO");
    if (path && *path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            error = std::string("cannot read MIB_SERIAL_SIM_SCENARIO=") + path;
            return false;
        }
        std::ostringstream text;
        text << in.rdbuf();
        try {
            rig = loadScenario(text.str(), seed);
        } catch (const std::exception& e) {
            error = std::string(path) + ": " + e.what();
            return false;
        }
    } else {
        rig = makeBenchRig(seed.value_or(1));
    }
    install(rig);
    SPDLOG_WARN("serial-sim: MIB_SERIAL_MODE=sim, scenario {}, ports: {}",
                path && *path ? path : "built-in bench", [&] {
                    std::string names;
                    for (const auto& p : rig->enumerate()) names += (names.empty() ? "" : ", ") + p.systemName;
                    return names.empty() ? std::string("(none enumerated yet)") : names;
                }());
    return true;
}

} // namespace backend::services::serialsim
