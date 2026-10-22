#include "backend/camera/features/FeatureControl.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace backend::camera::features;
namespace {
int checks = 0;
void check(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
struct FakePort : Port {
    Descriptor descriptor;
    int descriptions{0}, writes{0};
    bool throwWrite{false}, throwReadback{false}, readbackAvailable{true};
    std::optional<Value> actual;
    Descriptor describe(const Address&) override {
        ++descriptions;
        if (writes && throwReadback) throw std::runtime_error("readback transport failure");
        auto result = descriptor;
        if (writes) {
            result.readable = readbackAvailable;
            result.value = readbackAvailable ? actual : std::nullopt;
        }
        return result;
    }
    void write(const Address&, const Value& value) override {
        ++writes;
        if (throwWrite) throw std::runtime_error("write transport outcome unknown");
        if (!actual) actual = value;
    }
};
FakePort integerPort() {
    FakePort p;
    p.descriptor.kind = Kind::Integer;
    p.descriptor.implemented = p.descriptor.readable = p.descriptor.writable = true;
    p.descriptor.value = std::int64_t{3};
    p.descriptor.minimum = std::int64_t{3};
    p.descriptor.maximum = std::int64_t{99};
    p.descriptor.increment = std::int64_t{4};
    return p;
}
CurrentSession session{{"source-A", 9007199254740993ULL, 7}, true, {}};
Request request{session.token, {Module::Remote, "Width", std::nullopt}};
} // namespace
int main() {
    {
        auto p = integerPort();
        auto r = write(p, session, request, std::int64_t{11});
        check(r.outcome == Outcome::ReadBack && p.writes == 1 && p.descriptions == 2,
              "write refreshes before and after");
        check(std::get<std::int64_t>(*r.requested) == 11 &&
                  std::get<std::int64_t>(*r.readback->value) == 11,
              "requested and readback separate");
    }
    for (const auto which : {0, 1, 2}) {
        auto p = integerPort();
        auto stale = request;
        if (which == 0) stale.expected.source = "source-B";
        if (which == 1) --stale.expected.generation;
        if (which == 2) --stale.expected.capabilityRevision;
        check(write(p, session, stale, std::int64_t{11}).outcome == Outcome::Stale,
              "stale source/session/revision refused");
        check(p.writes == 0 && p.descriptions == 0, "stale request never reaches port");
    }
    {
        auto p = integerPort();
        auto viewer = session;
        viewer.writesAuthorized = false;
        viewer.writeBlockReason = "Viewer cannot write.";
        auto r = write(p, viewer, request, std::int64_t{11});
        check(r.outcome == Outcome::Refused && r.reason == viewer.writeBlockReason &&
                  p.descriptions == 0,
              "authorization before port access");
        check(read(p, viewer, request).outcome == Outcome::Read, "viewer can read");
    }
    for (const Value v : {Value{std::int64_t{12}}, Value{std::int64_t{2}}, Value{std::int64_t{100}},
                          Value{11.0}, Value{true}}) {
        auto p = integerPort();
        check(write(p, session, request, v).outcome == Outcome::Refused && p.writes == 0,
              "bounds/increment/type reject before write");
    }
    {
        auto p = integerPort();
        p.descriptor.writable = false;
        p.descriptor.accessReason = "Locked during acquisition.";
        auto r = write(p, session, request, std::int64_t{11});
        check(r.outcome == Outcome::Refused && r.reason == p.descriptor.accessReason && !p.writes,
              "fresh access state governs writes");
        p.descriptor.implemented = false;
        check(read(p, session, request).outcome == Outcome::Unsupported,
              "unimplemented is explicit");
        p.descriptor.implemented = true;
        p.descriptor.kind = Kind::Command;
        check(write(p, session, request, std::int64_t{11}).outcome == Outcome::Unsupported,
              "scalar writes never execute commands");
    }
    {
        auto p = integerPort();
        p.actual = std::int64_t{15};
        auto r = write(p, session, request, std::int64_t{11});
        check(r.outcome == Outcome::ReadBack && std::get<std::int64_t>(*r.readback->value) == 15 &&
                  std::get<std::int64_t>(*r.requested) == 11,
              "readback differing from requested is preserved");
    }
    {
        auto p = integerPort();
        p.readbackAvailable = false;
        auto r = write(p, session, request, std::int64_t{11});
        check(r.outcome == Outcome::WrittenUnconfirmed && !r.readback->value,
              "write-only acknowledgement is not confirmed value");
    }
    for (bool failureOnWrite : {false, true}) {
        auto p = integerPort();
        p.throwWrite = failureOnWrite;
        p.throwReadback = !failureOnWrite;
        auto r = write(p, session, request, std::int64_t{11});
        check(r.outcome == Outcome::UnknownWriteOutcome && !r.readback && p.writes == 1,
              "uncertain write clears old value and is never retried");
    }
    {
        auto p = integerPort();
        p.descriptor.minimum = std::numeric_limits<std::int64_t>::min();
        p.descriptor.maximum = std::numeric_limits<std::int64_t>::max();
        p.descriptor.increment = std::int64_t{3};
        check(!validateValue(p.descriptor, std::numeric_limits<std::int64_t>::max()),
              "full u64 span increment avoids signed overflow");
        check(validateValue(p.descriptor, std::numeric_limits<std::int64_t>::max() - 1).has_value(),
              "adjacent full-span value rejected");
        p.descriptor.minimum.reset();
        check(validateValue(p.descriptor, std::int64_t{3}).has_value(),
              "missing bounds are not unlimited");
    }
    {
        Descriptor d;
        d.kind = Kind::Float;
        d.minimum = 0.1;
        d.maximum = 1.0;
        d.increment = 0.1;
        check(!validateValue(d, 0.3), "floating roundoff on valid increment tolerated");
        check(validateValue(d, 0.35).has_value(), "off-grid float rejected");
        check(validateValue(d, std::numeric_limits<double>::quiet_NaN()).has_value(),
              "NaN rejected");
        check(validateValue(d, std::numeric_limits<double>::infinity()).has_value(),
              "infinity rejected");
        d.increment = 0.0;
        check(validateValue(d, 0.3).has_value(), "invalid device increment rejected");
        d.increment.reset();
        check(!validateValue(d, 0.35), "absent increment explicitly supported");
    }
    {
        Descriptor d;
        d.kind = Kind::Enumeration;
        d.entries = {"Off", "On"};
        check(!validateValue(d, std::string{"On"}), "available enum accepted");
        check(validateValue(d, std::string{"Auto"}).has_value(), "unavailable enum refused");
        d.kind = Kind::String;
        check(validateValue(d, std::string(4097, 'a')).has_value(), "string bounded");
        check(validateValue(d, std::string("a\0b", 3)).has_value(), "embedded NUL refused");
    }
    for (const auto* name :
         {"@checkRange", "Width.Min", "Width[IIN11]", "Width;AcquisitionStart", ""}) {
        auto p = integerPort();
        auto invalid = request;
        invalid.address.feature = name;
        check(read(p, session, invalid).outcome == Outcome::Refused && !p.descriptions,
              "virtual options/scripts cannot enter feature name");
    }
    {
        auto a = request.address;
        a.selectorValue = "IIN11";
        check(!validateAddress(a), "enumerated selector accepted");
        a.selectorValue = "-12";
        check(!validateAddress(a), "numeric selector accepted");
        a.selectorValue = "IIN11] @rollbackSelectors=False";
        check(validateAddress(a).has_value(), "selector syntax injection refused");
        a = request.address;
        a.module = static_cast<Module>(999);
        check(validateAddress(a).has_value(), "unknown module refused");
    }
    std::cout << checks << " feature contract checks passed\n";
}
