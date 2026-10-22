#include "backend/camera/features/FeatureControl.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <type_traits>

namespace backend::camera::features {

bool SessionToken::operator==(const SessionToken& other) const {
    return source == other.source && generation == other.generation &&
           capabilityRevision == other.capabilityRevision;
}

namespace {
bool identifier(const std::string& text) {
    const auto letter = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
    };
    if (text.empty() || text.size() > 128 || !letter(text.front())) return false;
    return std::all_of(text.begin(), text.end(),
                       [&](char c) { return letter(c) || (c >= '0' && c <= '9'); });
}
bool selector(const std::string& text) {
    if (identifier(text)) return true;
    if (text.empty() || text.size() > 20) return false;
    const auto begin = text.begin() + (text.front() == '-' ? 1 : 0);
    return begin != text.end() &&
           std::all_of(begin, text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

template <class T> std::optional<std::string> numeric(const Descriptor& d, T value) {
    if (!d.minimum || !d.maximum || !std::holds_alternative<T>(*d.minimum) ||
        !std::holds_alternative<T>(*d.maximum))
        return "Numeric bounds are unavailable or have the wrong type.";
    const auto low = std::get<T>(*d.minimum), high = std::get<T>(*d.maximum);
    if constexpr (std::is_same_v<T, double>) {
        if (!std::isfinite(value) || !std::isfinite(low) || !std::isfinite(high))
            return "Non-finite numeric value or bounds.";
    }
    if (low > high || value < low || value > high)
        return "Value is outside the current device bounds.";
    if (d.increment) {
        if (!std::holds_alternative<T>(*d.increment)) return "Increment has the wrong type.";
        const T step = std::get<T>(*d.increment);
        if (!(step > 0)) return "Increment must be positive.";
        if constexpr (std::is_same_v<T, std::int64_t>) {
            // Unsigned subtraction also handles INT64_MIN..INT64_MAX without
            // signed overflow. Alignment is relative to the device minimum.
            const auto offset = static_cast<std::uint64_t>(value) - static_cast<std::uint64_t>(low);
            if (offset % static_cast<std::uint64_t>(step))
                return "Value is not aligned to the current increment.";
        } else {
            if (!std::isfinite(step)) return "Increment must be finite.";
            const long double steps = (static_cast<long double>(value) - low) / step;
            const long double tolerance =
                std::min(1e-7L, 16 * std::numeric_limits<double>::epsilon() *
                                    std::max(1.0L, std::abs(steps)));
            if (!std::isfinite(steps) || std::abs(steps - std::round(steps)) > tolerance)
                return "Value is not aligned to the current increment.";
        }
    }
    return std::nullopt;
}

Result base(const CurrentSession& current, const Request& request) {
    Result r;
    r.session = current.token;
    r.address = request.address;
    return r;
}
bool precheck(Result& r, const CurrentSession& current, const Request& request) {
    if (current.token.source.empty() || current.token.generation == 0 ||
        current.token.capabilityRevision == 0 || !(request.expected == current.token)) {
        r.outcome = Outcome::Stale;
        r.reason = "Source, session or capability revision changed; refresh before retrying.";
        return false;
    }
    if (const auto invalid = validateAddress(request.address)) {
        r.outcome = Outcome::Refused;
        r.reason = *invalid;
        return false;
    }
    return true;
}
} // namespace

std::optional<std::string> validateAddress(const Address& address) {
    switch (address.module) {
    case Module::System:
    case Module::Interface:
    case Module::Device:
    case Module::Stream:
    case Module::Remote:
        break;
    default:
        return "Unknown GenApi module.";
    }
    if (!identifier(address.feature))
        return "Feature name must be a bounded GenApi identifier, not a script or virtual option.";
    if (address.selectorValue && !selector(*address.selectorValue))
        return "Unsupported selector value syntax.";
    return std::nullopt;
}

std::optional<std::string> validateValue(const Descriptor& d, const Value& value) {
    switch (d.kind) {
    case Kind::Integer:
        if (const auto* v = std::get_if<std::int64_t>(&value)) return numeric(d, *v);
        break;
    case Kind::Float:
        if (const auto* v = std::get_if<double>(&value)) return numeric(d, *v);
        break;
    case Kind::Boolean:
        if (std::holds_alternative<bool>(value)) return std::nullopt;
        break;
    case Kind::String:
    case Kind::Enumeration:
        if (const auto* v = std::get_if<std::string>(&value)) {
            if (v->size() > 4096 || v->find('\0') != std::string::npos)
                return "String value is too large or contains NUL.";
            if (d.kind == Kind::Enumeration &&
                std::find(d.entries.begin(), d.entries.end(), *v) == d.entries.end())
                return "Enumeration entry is not currently available.";
            return std::nullopt;
        }
        break;
    default:
        return "This feature kind is not a scalar control.";
    }
    return "Requested value type does not match the feature type.";
}

Result read(Port& port, const CurrentSession& current, const Request& request) {
    auto r = base(current, request);
    if (!precheck(r, current, request)) return r;
    try {
        r.readback = port.describe(request.address);
        r.outcome = r.readback->implemented ? Outcome::Read : Outcome::Unsupported;
        r.reason = r.readback->accessReason;
    } catch (const std::exception& e) {
        r.outcome = Outcome::Error;
        r.reason = e.what();
    }
    return r;
}

Result write(Port& port, const CurrentSession& current, const Request& request,
             const Value& value) {
    auto r = base(current, request);
    r.requested = value;
    if (!precheck(r, current, request)) return r;
    if (!current.writesAuthorized) {
        r.outcome = Outcome::Refused;
        r.reason = current.writeBlockReason.empty() ? "Writes are not authorized in this session."
                                                    : current.writeBlockReason;
        return r;
    }
    bool submitted = false;
    try {
        auto descriptor = port.describe(request.address);
        r.readback = descriptor;
        if (!descriptor.implemented || descriptor.kind == Kind::Unsupported ||
            descriptor.kind == Kind::Command) {
            r.outcome = Outcome::Unsupported;
            r.reason = "Feature is unimplemented or not a supported scalar control.";
            return r;
        }
        if (!descriptor.writable) {
            r.outcome = Outcome::Refused;
            r.reason = descriptor.accessReason.empty() ? "Feature is not currently writable."
                                                       : descriptor.accessReason;
            return r;
        }
        if (const auto invalid = validateValue(descriptor, value)) {
            r.outcome = Outcome::Refused;
            r.reason = *invalid;
            return r;
        }
        // A thrown write may have reached the device. Do not report refusal or
        // automatically retry it, and do not keep the pre-write value as readback.
        r.readback.reset();
        submitted = true;
        port.write(request.address, value);
        r.readback = port.describe(request.address);
        if (r.readback->readable && r.readback->value) {
            r.outcome = Outcome::ReadBack;
        } else {
            r.outcome = Outcome::WrittenUnconfirmed;
            r.reason = "Write acknowledged, but authoritative readback is unavailable.";
        }
    } catch (const std::exception& e) {
        r.outcome = submitted ? Outcome::UnknownWriteOutcome : Outcome::Error;
        r.reason = e.what();
    }
    return r;
}

} // namespace backend::camera::features
