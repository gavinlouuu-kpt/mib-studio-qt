#include "backend/nanopositioner/oeabt/OeabtProtocol.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <sstream>
#include <vector>

namespace backend::nanopositioner::oeabt {
namespace {

// SinglePiezo 1.5.0 emits the first form. The June 2026 O'motion command
// manual documents the second form, with an explicit Run instruction.
constexpr std::array<std::string_view, 2> kIdentityCommands{"/1&\r", "/1&R\r"};
constexpr std::string_view kIdentityMarker = "Oeabt pzt controller";
constexpr std::string_view kReadVoltageCommand = "/1?aVtR\r";
constexpr std::string_view kReadMaxVoltageCommand = "/1?aVtmR\r";
constexpr std::string_view kReadMaxStrokeCommand = "/1?aSR\r";
constexpr std::string_view kReadAnalogCapabilityCommand = "/1?et1R\r";
constexpr std::string_view kReadPwmCapabilityCommand = "/1?et2R\r";

Error makeError(ErrorCode code, std::string message, std::string raw = {}) {
    return Error{code, std::move(message), std::move(raw)};
}

std::string_view trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return value;
}

std::string_view statusErrorDescription(unsigned int code) {
    constexpr std::array<std::string_view, 16> descriptions{
        "no error",          "initialization error", "invalid command",
        "invalid operand",   "nesting limit",        "internal communication error",
        "command limit",     "not initialized",      "reserved error 8",
        "overload",          "reserved error 10",    "movement not allowed",
        "reserved error 12", "reserved error 13",    "fly-scan buffer overflow",
        "command overflow"};
    return descriptions.at(code & 0x0fU);
}

} // namespace

ControllerSession::ControllerSession(IByteTransport& transport) : transport_(transport) {}

Result<std::string> ControllerSession::transact(std::string_view command, bool retryReadOnly) {
    const int attempts = retryReadOnly ? 2 : 1;
    Error lastError = makeError(ErrorCode::IoError, "OEABT transaction did not run");

    for (int attempt = 0; attempt < attempts; ++attempt) {
        auto clearResult = transport_.clear();
        if (!clearResult) {
            return Result<std::string>::failure(clearResult.error());
        }

        auto writeResult = transport_.write(command);
        if (!writeResult) {
            return Result<std::string>::failure(writeResult.error());
        }

        auto readResult = transport_.readUntil('\n', kTransactionTimeout);
        if (readResult) {
            if (readResult.value().empty()) {
                lastError = makeError(ErrorCode::Timeout, "OEABT returned no response");
            } else {
                return readResult;
            }
        } else {
            lastError = readResult.error();
        }

        if (lastError.code != ErrorCode::Timeout) {
            break;
        }
    }

    return Result<std::string>::failure(std::move(lastError));
}

Result<std::string> ControllerSession::identify() {
    Error lastError = makeError(ErrorCode::Timeout, "OEABT returned no identity response");
    for (const auto command : kIdentityCommands) {
        auto response = transact(command, false);
        if (!response) {
            lastError = response.error();
            continue;
        }

        auto payload = parsePayload(response.value());
        if (!payload) {
            lastError = payload.error();
            continue;
        }
        if (payload.value().find(kIdentityMarker) != std::string::npos) {
            return Result<std::string>::success(kIdentityMarker.data());
        }
        lastError =
            makeError(ErrorCode::NotOeabt,
                      "Serial endpoint did not identify as an OEABT controller", response.value());
    }
    return Result<std::string>::failure(std::move(lastError));
}

Result<std::string> ControllerSession::parsePayload(std::string_view rawResponse) {
    if (rawResponse.size() < 7) {
        return Result<std::string>::failure(makeError(ErrorCode::ProtocolError,
                                                      "OEABT response is shorter than its framing",
                                                      std::string(rawResponse)));
    }

    const auto byte = [&](std::size_t index) {
        return static_cast<unsigned int>(static_cast<unsigned char>(rawResponse[index]));
    };
    // The O'motion manual specifies 0x1f for the second header byte while
    // simultaneously calling it ASCII '/', which is 0x2f. Accept both literal
    // interpretations until a physical response capture resolves the typo.
    const bool validHeader =
        byte(0) == 0xffU && (byte(1) == 0x1fU || byte(1) == 0x2fU) && byte(2) == 0x30U;
    const bool validTrailer = byte(rawResponse.size() - 3) == 0x03U &&
                              byte(rawResponse.size() - 2) == 0x0dU &&
                              byte(rawResponse.size() - 1) == 0x0aU;
    if (!validHeader || !validTrailer) {
        return Result<std::string>::failure(makeError(ErrorCode::ProtocolError,
                                                      "OEABT response has invalid framing",
                                                      std::string(rawResponse)));
    }

    const unsigned int status = byte(3);
    if ((status & 0x40U) == 0U || (status & 0x90U) != 0U) {
        return Result<std::string>::failure(makeError(ErrorCode::ProtocolError,
                                                      "OEABT response has invalid status flags",
                                                      std::string(rawResponse)));
    }
    const unsigned int deviceError = status & 0x0fU;
    if (deviceError != 0U) {
        std::ostringstream message;
        message << "OEABT controller status error " << deviceError << " ("
                << statusErrorDescription(deviceError) << ')';
        return Result<std::string>::failure(
            makeError(ErrorCode::ProtocolError, message.str(), std::string(rawResponse)));
    }
    if ((status & 0x20U) == 0U) {
        return Result<std::string>::failure(
            makeError(ErrorCode::Busy, "OEABT controller is not ready", std::string(rawResponse)));
    }

    return Result<std::string>::success(std::string(rawResponse.substr(4, rawResponse.size() - 7)));
}

Result<std::string> ControllerSession::queryPayload(std::string_view command) {
    auto response = transact(command, true);
    if (!response) {
        return response;
    }
    return parsePayload(response.value());
}

Result<std::string> ControllerSession::queryFirstField(std::string_view command,
                                                       std::size_t expectedFieldCount) {
    auto payload = queryPayload(command);
    if (!payload) {
        return payload;
    }

    if (expectedFieldCount == 0 || expectedFieldCount > 4) {
        return Result<std::string>::failure(
            makeError(ErrorCode::ProtocolError, "Invalid expected OEABT field count"));
    }

    // Older firmware returns scalar capabilities; V0.5.4 returns all four axes.
    if (expectedFieldCount == 1 && payload.value().find(',') != std::string::npos) {
        expectedFieldCount = 4;
    }

    std::array<std::string_view, 4> fields{};
    std::string_view remaining(payload.value());
    for (std::size_t i = 0; i < expectedFieldCount; ++i) {
        const auto comma = remaining.find(',');
        if (i + 1 == expectedFieldCount) {
            if (comma != std::string_view::npos) {
                return Result<std::string>::failure(makeError(ErrorCode::ProtocolError,
                                                              "OEABT response has too many fields",
                                                              payload.value()));
            }
            fields[i] = trim(remaining);
        } else {
            if (comma == std::string_view::npos) {
                return Result<std::string>::failure(makeError(ErrorCode::ProtocolError,
                                                              "OEABT response has too few fields",
                                                              payload.value()));
            }
            fields[i] = trim(remaining.substr(0, comma));
            remaining.remove_prefix(comma + 1);
        }
    }

    if (fields.front().empty()) {
        return Result<std::string>::failure(makeError(
            ErrorCode::ProtocolError, "OEABT response has an empty first field", payload.value()));
    }
    return Result<std::string>::success(std::string(fields.front()));
}

Result<std::int32_t> ControllerSession::parseInteger(std::string_view value,
                                                     std::string_view fieldName) {
    value = trim(value);
    std::int32_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
        return Result<std::int32_t>::failure(makeError(
            ErrorCode::ProtocolError, "OEABT " + std::string(fieldName) + " is not an integer",
            std::string(value)));
    }
    return Result<std::int32_t>::success(parsed);
}

Result<VoltageMv> ControllerSession::readVoltage() {
    auto field = queryFirstField(kReadVoltageCommand);
    if (!field) {
        return Result<VoltageMv>::failure(field.error());
    }
    auto value = parseInteger(field.value(), "voltage");
    if (!value) {
        return Result<VoltageMv>::failure(value.error());
    }
    return Result<VoltageMv>::success(VoltageMv{value.value()});
}

Result<Capabilities> ControllerSession::readCapabilities() {
    Capabilities capabilities;

    auto maxVoltageField = queryFirstField(kReadMaxVoltageCommand);
    if (!maxVoltageField) {
        return Result<Capabilities>::failure(maxVoltageField.error());
    }
    auto maxVoltage = parseInteger(maxVoltageField.value(), "maximum voltage");
    if (!maxVoltage || maxVoltage.value() <= 0) {
        return Result<Capabilities>::failure(
            maxVoltage
                ? makeError(ErrorCode::ProtocolError, "OEABT maximum voltage must be positive",
                            maxVoltageField.value())
                : maxVoltage.error());
    }
    capabilities.maxVoltage = VoltageMv{maxVoltage.value()};

    auto maxStroke = queryFirstField(kReadMaxStrokeCommand);
    if (!maxStroke) {
        return Result<Capabilities>::failure(maxStroke.error());
    }
    capabilities.maximumStrokeRaw = maxStroke.value();

    auto analogField = queryFirstField(kReadAnalogCapabilityCommand, 1);
    if (!analogField) {
        return Result<Capabilities>::failure(analogField.error());
    }
    auto analog = parseInteger(analogField.value(), "analog capability");
    if (!analog) {
        return Result<Capabilities>::failure(analog.error());
    }
    capabilities.analogControl = analog.value() != 0;

    auto pwmField = queryFirstField(kReadPwmCapabilityCommand, 1);
    if (!pwmField) {
        return Result<Capabilities>::failure(pwmField.error());
    }
    auto pwm = parseInteger(pwmField.value(), "PWM capability");
    if (!pwm) {
        return Result<Capabilities>::failure(pwm.error());
    }
    capabilities.pwmControl = pwm.value() != 0;

    maximumVoltage_.value = std::min(maximumVoltage_.value, capabilities.maxVoltage.value);
    return Result<Capabilities>::success(std::move(capabilities));
}

Result<void> ControllerSession::setMode(ControlMode mode) {
    std::string_view command;
    switch (mode) {
    case ControlMode::Manual:
        command = "/1aM1et0R\r";
        break;
    case ControlMode::Analog:
        command = "/1aM1et1R\r";
        break;
    case ControlMode::Pwm:
        command = "/1aM1et2R\r";
        break;
    }

    auto response = transact(command, false);
    if (!response) {
        return Result<void>::failure(response.error());
    }
    auto payload = parsePayload(response.value());
    if (!payload) {
        return Result<void>::failure(payload.error());
    }
    return Result<void>::success();
}

Result<void> ControllerSession::setSafetyLimits(VoltageMv minimum, VoltageMv maximum) {
    if (minimum.value < 0 || maximum.value < minimum.value) {
        return Result<void>::failure(
            makeError(ErrorCode::OutOfRange, "OEABT safety voltage limits are invalid"));
    }
    minimumVoltage_ = minimum;
    maximumVoltage_ = maximum;
    return Result<void>::success();
}

Result<void> ControllerSession::setVoltage(VoltageMv voltage) {
    if (voltage.value < minimumVoltage_.value || voltage.value > maximumVoltage_.value) {
        std::ostringstream message;
        message << "OEABT voltage " << voltage.value << " mV is outside the allowed range ["
                << minimumVoltage_.value << ", " << maximumVoltage_.value << "] mV";
        return Result<void>::failure(makeError(ErrorCode::OutOfRange, message.str()));
    }

    auto modeResult = setMode(ControlMode::Manual);
    if (!modeResult) {
        return modeResult;
    }

    const std::string command = "/1aM1Vt" + std::to_string(voltage.value) + "R\r";
    auto response = transact(command, false);
    if (!response) {
        return Result<void>::failure(response.error());
    }
    auto payload = parsePayload(response.value());
    if (!payload) {
        return Result<void>::failure(payload.error());
    }
    return Result<void>::success();
}

} // namespace backend::nanopositioner::oeabt
