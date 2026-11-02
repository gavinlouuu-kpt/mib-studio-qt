#pragma once

#include "backend/camera/features/FeatureControl.h"

// E02 (#806): a Port decorator that forwards every call and records whether an
// operation could have moved selector state in one module: a write there, or a
// selector-scoped describe there (the SDK sets the selector and rolls it back,
// so its value is moved for the duration of the call). EGrabberCamera wraps its
// port in one for Module::Interface, so only such an operation makes the next
// trigger pulse re-select its LineSelector; plain reads and every other module
// leave the trigger's selector state alone. Header-only and SDK-free.
namespace camera::features {

class ModuleWriteTracker final : public Port {
public:
    ModuleWriteTracker(Port& inner, Module watched) : inner_(inner), watched_(watched) {}

    // True once a write or a selector-scoped describe reached the watched module
    // (set before the call, so an operation that threw still counts).
    bool touched() const { return touched_; }

    Descriptor describe(const Address& address) override {
        if (address.module == watched_ && address.selectorValue) touched_ = true;
        return inner_.describe(address);
    }
    void write(const Address& address, const Value& value) override {
        if (address.module == watched_) touched_ = true;
        inner_.write(address, value);
    }
    bool canEnumerate() const override { return inner_.canEnumerate(); }
    std::vector<std::string> categories(Module module, bool availableOnly) override {
        return inner_.categories(module, availableOnly);
    }
    std::vector<std::string> categoriesOf(Module module, const std::string& category,
                                          bool availableOnly) override {
        return inner_.categoriesOf(module, category, availableOnly);
    }
    std::vector<std::string> featuresOf(Module module, const std::string& category,
                                        bool availableOnly) override {
        return inner_.featuresOf(module, category, availableOnly);
    }
    std::vector<std::string> features(Module module, bool availableOnly) override {
        return inner_.features(module, availableOnly);
    }
    NodeInfo info(Module module, const std::string& node) override {
        return inner_.info(module, node);
    }

private:
    Port& inner_;
    Module watched_;
    bool touched_{false};
};

} // namespace camera::features
