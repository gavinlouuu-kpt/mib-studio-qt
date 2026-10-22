#pragma once

#include "backend/camera/features/FeatureControl.h"
#include <EGrabber.h>

#include <algorithm>
#include <stdexcept>
#include <type_traits>

namespace backend::camera::features {

// Borrow an existing capture-owned EGrabber; never constructs/opens a device.
// The enclosing capture session must serialize this port with stop, scripts,
// selectors and other feature operations. This adapter has no shared state.
// SDK transaction semantics are documented in eGrabber 26.09, section 7.2.3.
template <class Grabber> class EGrabberFeaturePort final : public Port {
public:
    explicit EGrabberFeaturePort(Grabber& grabber) : grabber_(grabber) {}

    Descriptor describe(const Address& address) override {
        check(address);
        switch (address.module) {
        case Module::System:
            return describeModule<Euresys::SystemModule>(address);
        case Module::Interface:
            return describeModule<Euresys::InterfaceModule>(address);
        case Module::Device:
            return describeModule<Euresys::DeviceModule>(address);
        case Module::Stream:
            return describeModule<Euresys::StreamModule>(address);
        case Module::Remote:
            return describeModule<Euresys::RemoteModule>(address);
        }
        throw std::invalid_argument("Unknown module");
    }
    void write(const Address& address, const Value& value) override {
        check(address);
        switch (address.module) {
        case Module::System:
            return writeModule<Euresys::SystemModule>(address, value);
        case Module::Interface:
            return writeModule<Euresys::InterfaceModule>(address, value);
        case Module::Device:
            return writeModule<Euresys::DeviceModule>(address, value);
        case Module::Stream:
            return writeModule<Euresys::StreamModule>(address, value);
        case Module::Remote:
            return writeModule<Euresys::RemoteModule>(address, value);
        }
        throw std::invalid_argument("Unknown module");
    }

private:
    static void check(const Address& address) {
        if (const auto reason = validateAddress(address)) throw std::invalid_argument(*reason);
    }
    template <class M> std::string scoped(const Address& address) {
        if (!address.selectorValue) return address.feature;
        // Refuse, rather than mutating a module-wide option that scripts may
        // have changed. No separate selector writes or rollback emulation.
        if (!grabber_.template getInteger<M>("@rollbackSelectors"))
            throw std::runtime_error(
                "Selector rollback is disabled; restore SDK rollback before exploring selectors.");
        return address.feature + "[" + *address.selectorValue + "]";
    }
    template <class M>
    std::string optionalInfo(const std::string& feature, const std::string& name) {
        try {
            return grabber_.template getString<M>(Euresys::query::info(feature, name));
        } catch (const std::exception&) {
            return {}; // absent optional XML metadata, never a numeric default
        }
    }
    template <class M, class T> T number(const std::string& feature) {
        if constexpr (std::is_same_v<T, std::int64_t>)
            return grabber_.template getInteger<M>(feature);
        else
            return grabber_.template getFloat<M>(feature);
    }
    template <class M, class T> void numericDescriptor(Descriptor& d, const std::string& feature) {
        d.minimum = Value{number<M, T>(feature + ".Min")};
        d.maximum = Value{number<M, T>(feature + ".Max")};
        try {
            d.increment = Value{number<M, T>(feature + ".Inc")};
        } catch (const std::exception&) { /* SDK documents Inc as optional */
        }
        if (d.readable) d.value = Value{number<M, T>(feature)};
        try {
            d.unit = grabber_.template getString<M>(feature + ".Unit");
        } catch (const std::exception&) { /* optional unit */
        }
    }
    template <class M> Descriptor describeModule(const Address& address) {
        const auto feature = scoped<M>(address);
        Descriptor d;
        d.implemented = grabber_.template getInteger<M>(Euresys::query::implemented(feature)) != 0;
        if (!d.implemented) {
            d.accessReason = "Feature is not implemented by this source/module.";
            return d;
        }
        d.readable = grabber_.template getInteger<M>(Euresys::query::readable(feature)) != 0;
        d.writable = grabber_.template getInteger<M>(Euresys::query::writeable(feature)) != 0;
        if (!d.writable)
            d.accessReason = d.readable ? "Read-only in the current context."
                                        : "Unavailable in the current context.";
        d.visibility = optionalInfo<M>(address.feature, "Visibility");
        const auto interfaces =
            grabber_.template getStringList<M>(Euresys::query::interfaces(feature));
        const auto has = [&](const std::string& name) {
            return std::find(interfaces.begin(), interfaces.end(), name) != interfaces.end() ||
                   std::find(interfaces.begin(), interfaces.end(), "I" + name) != interfaces.end();
        };
        if (has("Command"))
            d.kind = Kind::Command;
        else if (has("Enumeration")) {
            d.kind = Kind::Enumeration;
            d.entries =
                grabber_.template getStringList<M>(Euresys::query::enumEntries(feature, true));
            if (d.entries.size() > 1024)
                throw std::runtime_error("Enumeration exceeds descriptor limit.");
            if (d.readable) d.value = grabber_.template getString<M>(feature);
        } else if (has("Boolean")) {
            d.kind = Kind::Boolean;
            if (d.readable) d.value = grabber_.template getInteger<M>(feature) != 0;
        } else if (has("Integer")) {
            d.kind = Kind::Integer;
            numericDescriptor<M, std::int64_t>(d, feature);
        } else if (has("Float")) {
            d.kind = Kind::Float;
            numericDescriptor<M, double>(d, feature);
        } else if (has("String")) {
            d.kind = Kind::String;
            if (d.readable) d.value = grabber_.template getString<M>(feature);
        } else {
            d.accessReason = "Node interface is not a supported scalar control.";
        }
        return d;
    }
    template <class M> void writeModule(const Address& address, const Value& value) {
        const auto feature = scoped<M>(address);
        // Preserve the SDK's final authority even if limits changed after describe.
        if (!grabber_.template getInteger<M>("@checkRange"))
            throw std::runtime_error("SDK range checking is disabled; typed writes refused.");
        std::visit(
            [&](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, double>)
                    grabber_.template setFloat<M>(feature, v);
                else if constexpr (std::is_same_v<T, std::string>)
                    grabber_.template setString<M>(feature, v);
                else
                    grabber_.template setInteger<M>(feature, static_cast<std::int64_t>(v));
            },
            value);
    }
    Grabber& grabber_;
};

} // namespace backend::camera::features
