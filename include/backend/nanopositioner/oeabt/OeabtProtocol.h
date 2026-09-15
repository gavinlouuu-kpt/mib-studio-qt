#pragma once

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace backend::nanopositioner::oeabt {

enum class ErrorCode {
    None,
    Busy,
    Timeout,
    NotOeabt,
    ProtocolError,
    OutOfRange,
    IoError,
    Unsupported,
};

struct Error {
    ErrorCode code{ErrorCode::None};
    std::string message;
    std::string rawResponse;
};

template <typename T> class Result {
public:
    static Result success(T value) { return Result(std::move(value)); }
    static Result failure(Error error) { return Result(std::move(error)); }

    explicit operator bool() const { return value_.has_value(); }
    const T& value() const { return *value_; }
    T& value() { return *value_; }
    const Error& error() const { return error_; }

private:
    explicit Result(T value) : value_(std::move(value)) {}
    explicit Result(Error error) : error_(std::move(error)) {}

    std::optional<T> value_;
    Error error_;
};

template <> class Result<void> {
public:
    static Result success() { return Result(true, {}); }
    static Result failure(Error error) { return Result(false, std::move(error)); }

    explicit operator bool() const { return ok_; }
    const Error& error() const { return error_; }

private:
    Result(bool ok, Error error) : ok_(ok), error_(std::move(error)) {}

    bool ok_{false};
    Error error_;
};

struct VoltageMv {
    std::int32_t value{0};

    friend bool operator==(VoltageMv lhs, VoltageMv rhs) { return lhs.value == rhs.value; }
    friend bool operator!=(VoltageMv lhs, VoltageMv rhs) { return !(lhs == rhs); }
};

enum class ControlMode {
    Manual,
    Analog,
    Pwm,
};

struct Capabilities {
    VoltageMv maxVoltage;
    std::string maximumStrokeRaw;
    bool analogControl{false};
    bool pwmControl{false};
};

class IByteTransport {
public:
    virtual ~IByteTransport() = default;

    virtual Result<void> clear() = 0;
    virtual Result<void> write(std::string_view bytes) = 0;
    virtual Result<std::string> readUntil(char delimiter, std::chrono::milliseconds timeout) = 0;
};

class ControllerSession {
public:
    static constexpr std::chrono::milliseconds kTransactionTimeout{500};

    explicit ControllerSession(IByteTransport& transport);

    Result<std::string> identify();
    Result<VoltageMv> readVoltage();
    Result<Capabilities> readCapabilities();
    Result<void> setMode(ControlMode mode);
    Result<void> setVoltage(VoltageMv voltage);
    Result<void> setSafetyLimits(VoltageMv minimum, VoltageMv maximum);

    static Result<std::string> parsePayload(std::string_view rawResponse);

private:
    Result<std::string> transact(std::string_view command, bool retryReadOnly);
    Result<std::string> queryPayload(std::string_view command);
    Result<std::string> queryFirstField(std::string_view command,
                                        std::size_t expectedFieldCount = 4);
    static Result<std::int32_t> parseInteger(std::string_view value, std::string_view fieldName);

    IByteTransport& transport_;
    VoltageMv minimumVoltage_{0};
    VoltageMv maximumVoltage_{std::numeric_limits<std::int32_t>::max()};
};

} // namespace backend::nanopositioner::oeabt
