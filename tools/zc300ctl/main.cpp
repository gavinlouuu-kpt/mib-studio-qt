// zc300ctl: diagnostic CLI for the Zolix ZC300 stage controller (#464, ADR 0013).
//
// Read-only unless asked: `info` and `status` never write; `stop` is always
// allowed; `move` needs --allow-motion and `configure` needs --allow-write.
// Positions are the controller's own counter: referencing (Home) belongs to
// StageService, so CLI positions are unreferenced.

#include "backend/services/SerialBus.h"
#include "backend/stage/LimitVerification.h"
#include "backend/stage/zc300/Zc300Stage.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#if defined(_WIN32)
#include <conio.h>
#else
#include <poll.h>
#include <unistd.h>
#endif

using namespace backend::stage;
using backend::services::serialbus::SerialBusManager;

namespace {

std::atomic<bool> gInterrupted{false};

void onSignal(int) { gInterrupted.store(true); }

void usage()
{
    std::cerr << "Usage:\n"
              << "  zc300ctl list [--all]\n"
              << "  zc300ctl info   (--port <name> | --usb-serial <sn>) [--address N] [--profile tbzf6-60]\n"
              << "  zc300ctl status (--port <name> | --usb-serial <sn>) [--address N]\n"
              << "  zc300ctl stop   (--port <name> | --usb-serial <sn>) [--address N]\n"
              << "  zc300ctl move   (--port <name> | --usb-serial <sn>) (--to <um> | --by <um>)\n"
              << "                  --allow-motion [--timeout-s N]\n"
              << "  zc300ctl configure (--port <name> | --usb-serial <sn>) --profile tbzf6-60 --allow-write\n"
              << "  zc300ctl verify-limits (--port <name> | --usb-serial <sn>) --data-dir <app data dir>\n"
              << "                  --supervised --allow-motion [--speed-um-s 200] [--step-um 500]\n"
              << "      Supervised limit-switch check; with someone watching the stage. Home is refused\n"
              << "      until it passes for the controller. Enter or Ctrl-C stops the stage.\n"
              << "Distances are whole micrometres.\n";
}

std::optional<std::string> optionValue(int argc, char** argv, std::string_view name)
{
    for (int i = 2; i + 1 < argc; ++i) {
        if (argv[i] == name) return std::string(argv[i + 1]);
    }
    return std::nullopt;
}

bool hasFlag(int argc, char** argv, std::string_view name)
{
    for (int i = 2; i < argc; ++i) {
        if (argv[i] == name) return true;
    }
    return false;
}

std::optional<double> parseNumber(const std::optional<std::string>& text)
{
    if (!text) return std::nullopt;
    try {
        std::size_t used = 0;
        const double v = std::stod(*text, &used);
        if (used != text->size() || !std::isfinite(v)) return std::nullopt;
        return v;
    } catch (...) {
        return std::nullopt;
    }
}

int fail(std::string_view what, StageError error, const std::string& detail = {})
{
    std::cerr << "zc300ctl: " << what << ": " << toString(error);
    if (!detail.empty()) std::cerr << " (" << detail << ')';
    std::cerr << '\n';
    return 4;
}

void printStatus(const StageStatus& s)
{
    std::cout << std::fixed << std::setprecision(2) << "position: " << s.positionUm
              << " um (controller counter; the CLI does not track the operator's zero)\n"
              << "state: "
              << (s.state == MoveState::Moving ? "moving" : s.state == MoveState::Faulted ? "faulted" : "idle")
              << "\nlimits: negative=" << s.limitNegative << " positive=" << s.limitPositive
              << " home=" << s.home << "\nemergency stop: " << s.emergencyStop
              << "\ndriver alarm: " << s.driverAlarm << '\n';
}

// True once a line arrived on stdin (Enter) or Ctrl-C was pressed.
bool stopRequested()
{
    if (gInterrupted.load()) return true;
#if defined(_WIN32)
    if (_kbhit()) {
        _getch();
        return true;
    }
    return false;
#else
    struct pollfd pfd{STDIN_FILENO, POLLIN, 0};
    if (::poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
        std::string line;
        std::getline(std::cin, line);
        return true;
    }
    return false;
#endif
}

std::string utcNow()
{
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

int verifyLimitsCommand(int argc, char** argv, zc300::Zc300Stage& stage, const StageIdentity& identity,
                        const StageProfile& profile)
{
    const auto dataDir = optionValue(argc, argv, "--data-dir");
    if (!dataDir || dataDir->empty()) {
        std::cerr << "zc300ctl: verify-limits needs --data-dir (the application's data directory)\n";
        return 2;
    }
    if (!stage.isConfigured()) return fail("verify-limits", StageError::Misconfigured, "run configure first");
    LimitVerificationOptions o;
    o.searchSpeedUmS = parseNumber(optionValue(argc, argv, "--speed-um-s")).value_or(200.0);
    o.stepUm = parseNumber(optionValue(argc, argv, "--step-um")).value_or(500.0);
    if (!(o.searchSpeedUmS > 0.0 && o.searchSpeedUmS <= 500.0) || !(o.stepUm >= 10.0 && o.stepUm <= 500.0)) {
        std::cerr << "zc300ctl: --speed-um-s must be in (0, 500] and --step-um in [10, 500]\n";
        return 2;
    }
    o.expectedSpanUm = profile.travelUm;
    o.maxTravelUm = profile.travelUm + 500.0;
    o.confirm = [](const std::string& prompt) {
        std::cout << "\n" << prompt << "\nType 'yes' to move, anything else to abort: " << std::flush;
        std::string answer;
        if (!std::getline(std::cin, answer)) return false;
        return answer == "yes";
    };
    o.cancelled = [] { return stopRequested(); };
    o.report = [](const std::string& text) { std::cout << "  " << text << std::endl; };

    std::cout << "Supervised limit-switch check for " << identity.model << " s/n " << identity.serial << ".\n"
              << "The stage moves slowly toward each limit switch in small steps. Watch it; press Enter or\n"
              << "Ctrl-C to stop at once. It returns to the start position at the end.\n";
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    const auto result = verifyLimits(stage, o);
    if (result.cancelled) {
        stage.stop();
        std::cerr << "zc300ctl: stopped" << (result.detail.empty() ? "" : ": " + result.detail)
                  << "; nothing recorded\n";
        return 130;
    }
    if (!result.passed()) return fail("verify-limits", result.error, result.detail + "; nothing recorded");

    LimitsVerificationStore store((std::filesystem::path(*dataDir) / "stage_limits_verified.json").string());
    LimitsVerifiedRecord record{identity.serial, utcNow(), result.negativeUm, result.positiveUm, result.spanUm,
                                "zc300ctl verify-limits"};
    if (!store.save(record)) {
        std::cerr << "zc300ctl: the check passed but " << store.path() << " could not be written\n";
        return 4;
    }
    std::cout << "Limit switches verified (span " << std::lround(result.spanUm) << " um"
              << (result.returnedToStart ? ", back at the start position" : "") << "). Recorded in "
              << store.path() << "; Home is now allowed for this controller.\n";
    return 0;
}

int list(bool all)
{
    for (const auto& port : backend::services::serialbus::availablePorts()) {
        if (!all && port.vendorId == 0 && port.productId == 0) continue; // built-in UARTs
        const bool ftdi = port.vendorId == 0x0403 && port.productId == 0x6001;
        std::cout << (ftdi ? "ftdi    " : "serial  ") << port.systemName << "  usb-serial="
                  << port.serialNumber << "  " << port.description << '\n';
    }
    std::cout << "(a ZC300-1A uses an FTDI FT232R; identity is confirmed only by `info`)\n";
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        usage();
        return 2;
    }
    const std::string_view command = argv[1];
    if (command == "list") return list(hasFlag(argc, argv, "--all"));

    StageEndpoint endpoint;
    endpoint.systemPort = optionValue(argc, argv, "--port").value_or("");
    endpoint.usbSerial = optionValue(argc, argv, "--usb-serial").value_or("");
    if (const auto address = parseNumber(optionValue(argc, argv, "--address"))) {
        if (*address < 1 || *address > 255) {
            usage();
            return 2;
        }
        endpoint.modbusAddress = static_cast<std::uint8_t>(*address);
    }
    if (endpoint.systemPort.empty() && endpoint.usbSerial.empty()) {
        usage();
        return 2;
    }
    const std::string profileName = optionValue(argc, argv, "--profile").value_or("tbzf6-60");
    const auto profile = findStageProfile(profileName);
    if (!profile) {
        std::cerr << "zc300ctl: unknown profile '" << profileName << "'\n";
        return 2;
    }
    // Check the safety flags before touching the port.
    if (command == "move" && !hasFlag(argc, argv, "--allow-motion")) {
        std::cerr << "zc300ctl: move requires --allow-motion\n";
        return 6;
    }
    if (command == "verify-limits" &&
        (!hasFlag(argc, argv, "--supervised") || !hasFlag(argc, argv, "--allow-motion"))) {
        std::cerr << "zc300ctl: verify-limits moves the stage to both limit switches; it requires --supervised\n"
                     "          (someone is watching the stage) and --allow-motion\n";
        return 6;
    }
    if (command == "configure" && !hasFlag(argc, argv, "--allow-write")) {
        std::cerr << "zc300ctl: configure writes and saves controller parameters; it requires --allow-write\n";
        return 6;
    }

    SerialBusManager manager;
    zc300::Zc300Stage stage(manager);
    StageIdentity identity;
    std::string detail;
    if (const StageError err = stage.connect(endpoint, *profile, identity, detail); err != StageError::None) {
        return fail("connect", err, detail);
    }
    std::cout << "controller: " << identity.model << "  serial " << identity.serial << "  firmware "
              << identity.firmware << '\n';

    if (command == "info") {
        const auto config = stage.controllerConfig();
        const auto cal = stage.calibration();
        std::cout << "unit: " << static_cast<int>(config.unit) << " (0 pp, 1 mm, 2 deg)\n"
                  << "stage type: " << static_cast<int>(config.stageType) << " (0 linear, 1 rotary)\n"
                  << "lead: " << config.leadMm << " mm/rev\npulses/rev: " << config.pulsesPerRev << '\n'
                  << "resolution: " << std::setprecision(4) << cal.umPerPulse << " um/pulse\n"
                  << "profile '" << profile->name << "': "
                  << (stage.isConfigured() ? "matches" : "DOES NOT MATCH (motion refused; see `configure`)")
                  << '\n';
        if (!detail.empty()) std::cout << detail << '\n';
        return 0;
    }
    if (command == "status") {
        StageStatus s;
        if (const StageError err = stage.readStatus(s); err != StageError::None) return fail("status", err);
        printStatus(s);
        return 0;
    }
    if (command == "stop") {
        if (const StageError err = stage.stop(); err != StageError::None) return fail("stop", err);
        std::cout << "stopped\n";
        return 0;
    }
    if (command == "configure") {
        if (const StageError err = stage.applyProfile(*profile); err != StageError::None) {
            return fail("configure", err);
        }
        std::cout << "profile '" << profile->name << "' applied and saved to the controller\n";
        return 0;
    }
    if (command == "verify-limits") return verifyLimitsCommand(argc, argv, stage, identity, *profile);
    if (command == "move") {
        const auto to = parseNumber(optionValue(argc, argv, "--to"));
        const auto by = parseNumber(optionValue(argc, argv, "--by"));
        if (to.has_value() == by.has_value()) {
            usage();
            return 2;
        }
        if (!stage.isConfigured()) return fail("move", StageError::Misconfigured, detail);
        const int timeoutS = static_cast<int>(parseNumber(optionValue(argc, argv, "--timeout-s")).value_or(30));
        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);
        const StageError err = to ? stage.moveAbsolute(*to) : stage.moveRelative(*by);
        if (err != StageError::None) return fail("move", err);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutS);
        StageStatus s;
        while (true) {
            if (gInterrupted.load()) {
                stage.stop();
                std::cerr << "zc300ctl: interrupted; stop sent\n";
                return 130;
            }
            if (std::chrono::steady_clock::now() > deadline) {
                stage.stop();
                std::cerr << "zc300ctl: move did not finish within " << timeoutS << " s; stop sent\n";
                return 5;
            }
            if (const StageError e = stage.readStatus(s); e != StageError::None) {
                stage.stop();
                return fail("status during move", e);
            }
            if (s.state != MoveState::Moving) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        printStatus(s);
        return 0;
    }
    usage();
    return 2;
}
