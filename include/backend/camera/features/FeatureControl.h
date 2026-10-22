#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// E02: synchronous typed operations over an already-owned camera port. This
// module owns no device, session, thread or bridge ABI. The caller must hold its
// existing session/port serialization guard throughout each operation (including
// obtaining CurrentSession); never pass a camera pointer beyond that scope.
namespace backend::camera::features {

enum class Module { System, Interface, Device, Stream, Remote };
enum class Kind { Integer, Float, Boolean, String, Enumeration, Command, Unsupported };
using Value = std::variant<std::int64_t, double, bool, std::string>;

struct SessionToken {
    std::string source;
    std::uint64_t generation{0};
    std::uint64_t capabilityRevision{0};
    bool operator==(const SessionToken& other) const;
};

struct Address {
    Module module{Module::Remote};
    std::string feature;
    // One SDK-inferred selector slot, e.g. LineStatus[IIN11]. Multi-selector
    // enumeration is not claimed here; extend only with verified SDK syntax.
    std::optional<std::string> selectorValue;
};

struct Request {
    SessionToken expected;
    Address address;
};

struct CurrentSession {
    SessionToken token;
    bool writesAuthorized{false};
    std::string writeBlockReason;
};

struct Descriptor {
    Kind kind{Kind::Unsupported};
    bool implemented{false};
    bool readable{false};
    bool writable{false};
    std::string accessReason;
    std::string visibility;
    std::string unit;
    std::optional<Value> value;
    std::optional<Value> minimum;
    std::optional<Value> maximum;
    std::optional<Value> increment;
    std::vector<std::string> entries;
};

// The port implementation must refresh access/constraints on every describe;
// implement selector access atomically with restoration, not separate set/read
// calls. write must retain the SDK's range/access checks. Optional metadata may
// be absent; unsupported is not a readable zero. Commands use a separately gated
// engineering API, never this scalar write entry point.
class Port {
public:
    virtual ~Port() = default;
    virtual Descriptor describe(const Address& address) = 0;
    virtual void write(const Address& address, const Value& value) = 0;
};

enum class Outcome {
    Read,
    ReadBack,
    WrittenUnconfirmed,
    Refused,
    Stale,
    Unsupported,
    Error,
    UnknownWriteOutcome
};
struct Result {
    Outcome outcome{Outcome::Error};
    SessionToken session;
    Address address;
    std::optional<Value> requested;
    std::optional<Descriptor> readback;
    std::string reason;
};

// Pure validation; never parses arbitrary SDK scripts or configuration options.
std::optional<std::string> validateAddress(const Address& address);
std::optional<std::string> validateValue(const Descriptor& descriptor, const Value& value);
Result read(Port& port, const CurrentSession& current, const Request& request);
Result write(Port& port, const CurrentSession& current, const Request& request, const Value& value);

} // namespace backend::camera::features
