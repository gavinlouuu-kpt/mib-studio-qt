#include "backend/nanopositioner/oeabt/OeabtProtocol.h"
#include "backend/nanopositioner/oeabt/QtSerialTransport.h"

#include <QCoreApplication>

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

using backend::nanopositioner::oeabt::Capabilities;
using backend::nanopositioner::oeabt::ControllerSession;
using backend::nanopositioner::oeabt::QtSerialTransport;
using backend::nanopositioner::oeabt::VoltageMv;

namespace {

std::atomic<bool> gRunning{true};
constexpr std::int32_t kReadbackToleranceMv = 20;

void onSignal(int) {
    gRunning.store(false);
}

void usage() {
    std::cerr << "Usage:\n"
              << "  oeabtctl list [--json]\n"
              << "  oeabtctl probe --port <endpoint>\n"
              << "  oeabtctl status --port <endpoint>\n"
              << "  oeabtctl monitor --port <endpoint> [--samples <count>]\n"
              << "  oeabtctl set-voltage --port <endpoint> --volts <V> --allow-write\n"
              << "  oeabtctl verify-write --port <endpoint> --target-volts <V> --allow-write\n";
}

std::optional<std::string> optionValue(int argc, char** argv, std::string_view name) {
    for (int i = 2; i + 1 < argc; ++i) {
        if (argv[i] == name) {
            return std::string(argv[i + 1]);
        }
    }
    return std::nullopt;
}

bool hasFlag(int argc, char** argv, std::string_view name) {
    for (int i = 2; i < argc; ++i) {
        if (argv[i] == name) {
            return true;
        }
    }
    return false;
}

std::string jsonEscape(std::string_view value) {
    std::string escaped;
    for (char ch : value) {
        switch (ch) {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            escaped += ch;
            break;
        }
    }
    return escaped;
}

int printError(const backend::nanopositioner::oeabt::Error& error) {
    std::cerr << "oeabtctl: " << error.message;
    if (!error.rawResponse.empty()) {
        std::cerr << " (raw response length " << error.rawResponse.size() << ')';
    }
    std::cerr << '\n';
    return 4;
}

std::optional<VoltageMv> parseVoltage(std::string_view text) {
    try {
        std::size_t consumed = 0;
        const double volts = std::stod(std::string(text), &consumed);
        const double millivolts = std::trunc(volts * 1000.0);
        if (consumed != text.size() || !std::isfinite(volts) || millivolts < 0.0 ||
            millivolts > std::numeric_limits<std::int32_t>::max()) {
            return std::nullopt;
        }
        return VoltageMv{static_cast<std::int32_t>(millivolts)};
    } catch (...) {
        return std::nullopt;
    }
}

void printVoltage(std::string_view label, VoltageMv voltage) {
    std::cout << label << std::fixed << std::setprecision(3)
              << static_cast<double>(voltage.value) / 1000.0 << " V\n";
}

bool readbackMatches(VoltageMv expected, VoltageMv actual) {
    const auto difference = static_cast<std::int64_t>(expected.value) - actual.value;
    return std::abs(difference) <= kReadbackToleranceMv;
}

struct ConnectedController {
    explicit ConnectedController(std::string path)
        : transport(std::move(path)), session(transport) {}

    QtSerialTransport transport;
    ControllerSession session;
};

int openAndIdentify(ConnectedController& controller) {
    const auto opened = controller.transport.open();
    if (!opened) {
        return printError(opened.error());
    }
    const auto identity = controller.session.identify();
    if (!identity) {
        return printError(identity.error());
    }
    return 0;
}

int listEndpoints(bool json) {
    const auto endpoints = QtSerialTransport::enumerateEndpoints();
    if (json) {
        std::cout << '[';
        for (std::size_t i = 0; i < endpoints.size(); ++i) {
            const auto& endpoint = endpoints[i];
            if (i != 0) std::cout << ',';
            std::cout << "{\"id\":\"" << jsonEscape(endpoint.persistentId) << "\",\"path\":\""
                      << jsonEscape(endpoint.systemPath) << "\",\"display_name\":\""
                      << jsonEscape(endpoint.displayName) << "\",\"known_candidate\":"
                      << (endpoint.knownOeabtCandidate ? "true" : "false") << '}';
        }
        std::cout << "]\n";
        return 0;
    }

    for (const auto& endpoint : endpoints) {
        std::cout << (endpoint.knownOeabtCandidate ? "candidate " : "serial    ")
                  << endpoint.systemPath << "\n  id: " << endpoint.persistentId << "\n  "
                  << endpoint.displayName << '\n';
    }
    return 0;
}

int printStatus(ControllerSession& session, Capabilities* capabilitiesOut = nullptr,
                VoltageMv* voltageOut = nullptr) {
    const auto capabilities = session.readCapabilities();
    if (!capabilities) return printError(capabilities.error());
    const auto voltage = session.readVoltage();
    if (!voltage) return printError(voltage.error());

    printVoltage("voltage: ", voltage.value());
    printVoltage("maximum voltage: ", capabilities.value().maxVoltage);
    std::cout << "maximum stroke (raw): " << capabilities.value().maximumStrokeRaw << '\n'
              << "analog control: " << (capabilities.value().analogControl ? "yes" : "no") << '\n'
              << "PWM control: " << (capabilities.value().pwmControl ? "yes" : "no") << '\n';
    if (capabilitiesOut) *capabilitiesOut = capabilities.value();
    if (voltageOut) *voltageOut = voltage.value();
    return 0;
}

int commandWithPort(int argc, char** argv, std::string_view command) {
    const auto port = optionValue(argc, argv, "--port");
    if (!port) {
        usage();
        return 2;
    }

    ConnectedController controller(*port);
    if (const int result = openAndIdentify(controller); result != 0) {
        return result;
    }
    std::cout << "identity: Oeabt pzt controller\n";

    if (command == "probe") {
        return 0;
    }
    if (command == "status") {
        return printStatus(controller.session);
    }
    if (command == "monitor") {
        int samples = 0;
        if (const auto value = optionValue(argc, argv, "--samples")) {
            try {
                samples = std::stoi(*value);
            } catch (...) {
                std::cerr << "oeabtctl: --samples must be a non-negative integer\n";
                return 2;
            }
            if (samples < 0) return 2;
        }
        std::signal(SIGINT, onSignal);
        int emitted = 0;
        while (gRunning.load() && (samples == 0 || emitted < samples)) {
            const auto voltage = controller.session.readVoltage();
            if (!voltage) return printError(voltage.error());
            printVoltage("voltage: ", voltage.value());
            ++emitted;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return 0;
    }

    if (!hasFlag(argc, argv, "--allow-write")) {
        std::cerr << "oeabtctl: mutating commands require --allow-write\n";
        return 6;
    }
    const auto voltageOption =
        optionValue(argc, argv, command == "verify-write" ? "--target-volts" : "--volts");
    const auto target = voltageOption ? parseVoltage(*voltageOption) : std::nullopt;
    if (!target) {
        std::cerr << "oeabtctl: provide a valid non-negative voltage\n";
        return 2;
    }

    Capabilities capabilities;
    VoltageMv original;
    if (const int result = printStatus(controller.session, &capabilities, &original); result != 0) {
        return result;
    }
    const auto limits = controller.session.setSafetyLimits(VoltageMv{0}, capabilities.maxVoltage);
    if (!limits) return printError(limits.error());

    const auto setResult = controller.session.setVoltage(*target);
    if (!setResult) {
        return printError(setResult.error());
    }
    const auto readback = controller.session.readVoltage();
    if (!readback) {
        if (command == "verify-write") {
            const auto restore = controller.session.setVoltage(original);
            if (!restore) printError(restore.error());
        }
        return printError(readback.error());
    }
    printVoltage("readback: ", readback.value());

    const bool targetMatches = readbackMatches(*target, readback.value());

    if (command == "verify-write") {
        const auto restore = controller.session.setVoltage(original);
        if (!restore) return printError(restore.error());
        const auto restored = controller.session.readVoltage();
        if (!restored) return printError(restored.error());
        printVoltage("restored: ", restored.value());
        if (!targetMatches) {
            std::cerr << "oeabtctl: target readback differs by more than 0.020 V\n";
            return 6;
        }
        if (!readbackMatches(original, restored.value())) {
            std::cerr << "oeabtctl: restored voltage differs by more than 0.020 V\n";
            return 6;
        }
    } else if (!targetMatches) {
        std::cerr << "oeabtctl: voltage readback differs by more than 0.020 V\n";
        return 6;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    if (argc < 2) {
        usage();
        return 2;
    }

    const std::string_view command(argv[1]);
    if (command == "list") {
        return listEndpoints(hasFlag(argc, argv, "--json"));
    }
    if (command == "probe" || command == "status" || command == "monitor" ||
        command == "set-voltage" || command == "verify-write") {
        return commandWithPort(argc, argv, command);
    }

    usage();
    return 2;
}
