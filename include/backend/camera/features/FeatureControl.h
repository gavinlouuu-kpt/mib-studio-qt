#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

// E02: synchronous typed operations over an already-owned camera port. This
// module owns no device, session, thread or bridge ABI. The caller must hold its
// existing session/port serialization guard throughout each operation (including
// obtaining CurrentSession); never pass a camera pointer beyond that scope.
namespace camera::features {

enum class Module { System, Interface, Device, Stream, Remote };
enum class Kind { Integer, Float, Boolean, String, Enumeration, Command, Unsupported };
using Value = std::variant<std::int64_t, double, bool, std::string>;

struct SessionToken {
    std::string source;
    std::uint64_t generation{0};
    std::uint64_t capabilityRevision{0};
    // E01 (E-4): the backend instance that issued the token, a random UUID
    // drawn when the capture service is constructed. Generations restart at 1
    // in a new backend instance; this member is what tells a token issued
    // before a backend restart from one issued after it. Empty is never current.
    std::string instance;
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
    // Provenance class of the port behind this session: "device" for a real
    // GenApi node map, "fixture/mock" for a fake one. Never upgraded by callers.
    std::string evidence;
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

// Optional XML metadata of one node (feature or category), read from the node
// map. Empty means the node does not declare it, never a default.
struct NodeInfo {
    std::string displayName;
    std::string toolTip;
    std::string description;
    std::string visibility;
};

// The port implementation must refresh access/constraints on every describe;
// implement selector access atomically with restoration, not separate set/read
// calls. write must retain the SDK's range/access checks. Optional metadata may
// be absent; unsupported is not a readable zero. Commands use a separately gated
// engineering API, never this scalar write entry point.
//
// E-1 (E02) enumeration: every name below comes from the connected node map at
// call time; a port never answers from a hand-kept list. `availableOnly` false
// asks for everything the module exposes (access is then per descriptor). A
// port without introspection keeps canEnumerate() false and the enumeration
// calls throw, so a caller reports "unsupported", never an empty tree.
// Thrown by a Port's enumeration calls when the node map has no such module
// at all, as opposed to a module that is present but failed to enumerate.
// FeatureTree reports such a module as absent (enumerated false with the
// reason), never as an empty enumerated tree.
class ModuleAbsent : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Port {
public:
    virtual ~Port() = default;
    virtual Descriptor describe(const Address& address) = 0;
    virtual void write(const Address& address, const Value& value) = 0;

    virtual bool canEnumerate() const { return false; }
    // Every category of the module (flat).
    virtual std::vector<std::string> categories(Module module, bool availableOnly);
    // Direct child categories / direct features of one category.
    virtual std::vector<std::string> categoriesOf(Module module, const std::string& category,
                                                  bool availableOnly);
    virtual std::vector<std::string> featuresOf(Module module, const std::string& category,
                                                bool availableOnly);
    // Every feature of the module (flat), including any outside every category.
    virtual std::vector<std::string> features(Module module, bool availableOnly);
    virtual NodeInfo info(Module module, const std::string& node);
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
    std::string evidence; // CurrentSession::evidence of the session that answered
};

// Pure validation; never parses arbitrary SDK scripts or configuration options.
std::optional<std::string> validateAddress(const Address& address);
std::optional<std::string> validateValue(const Descriptor& descriptor, const Value& value);
Result read(Port& port, const CurrentSession& current, const Request& request);
Result write(Port& port, const CurrentSession& current, const Request& request, const Value& value);

} // namespace camera::features
