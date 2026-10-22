#include "backend/camera/features/EGrabberFeaturePort.h"
#include <cstdlib>
#include <iostream>
#include <typeindex>

using namespace backend::camera::features;
struct FakeGrabber {
    bool rollback{true}, range{true};
    int writes{0};
    std::string written;
    std::type_index module{typeid(void)};
    template <class M> std::int64_t getInteger(const std::string& key) {
        module = typeid(M);
        if (key == "@rollbackSelectors") return rollback;
        if (key == "@checkRange") return range;
        if (key == "Gain[A].Min") return 0;
        if (key == "Gain[A].Max") return 10;
        if (key == "Gain[A].Inc") return 1;
        if (key == "Gain[A]") return 5;
        if (key == std::string(Euresys::query::implemented("Gain[A]")) ||
            key == std::string(Euresys::query::readable("Gain[A]")) ||
            key == std::string(Euresys::query::writeable("Gain[A]")))
            return 1;
        throw std::runtime_error("Unexpected integer query: " + key);
    }
    template <class M> double getFloat(const std::string&) {
        throw std::runtime_error("unexpected float");
    }
    template <class M> std::string getString(const std::string&) { return {}; }
    template <class M> std::vector<std::string> getStringList(const std::string& key) {
        module = typeid(M);
        if (key == std::string(Euresys::query::interfaces("Gain[A]"))) return {"IInteger"};
        throw std::runtime_error("unexpected list");
    }
    template <class M> void setInteger(const std::string& key, std::int64_t) {
        module = typeid(M);
        written = key;
        ++writes;
    }
    template <class M> void setFloat(const std::string& key, double) {
        module = typeid(M);
        written = key;
        ++writes;
    }
    template <class M> void setString(const std::string& key, const std::string&) {
        module = typeid(M);
        written = key;
        ++writes;
    }
};
void check(bool ok) {
    if (!ok) std::exit(1);
}
int main() {
    FakeGrabber g;
    EGrabberFeaturePort<FakeGrabber> port(g);
    Address a{Module::Remote, "Gain", "A"};
    auto d = port.describe(a);
    check(d.kind == Kind::Integer && std::get<std::int64_t>(*d.value) == 5);
    check(g.module == typeid(Euresys::RemoteModule));
    const Module modules[]{Module::System, Module::Interface, Module::Device, Module::Stream,
                           Module::Remote};
    const std::type_index types[]{typeid(Euresys::SystemModule), typeid(Euresys::InterfaceModule),
                                  typeid(Euresys::DeviceModule), typeid(Euresys::StreamModule),
                                  typeid(Euresys::RemoteModule)};
    for (int i = 0; i < 5; ++i) {
        a.module = modules[i];
        port.write(a, std::int64_t{6});
        check(g.module == types[i] && g.written == "Gain[A]");
    }
    auto refused = [&] {
        const int before = g.writes;
        bool threw = false;
        try {
            port.write(a, std::int64_t{6});
        } catch (const std::exception&) {
            threw = true;
        }
        check(threw && g.writes == before);
    };
    g.rollback = false;
    refused();
    g.rollback = true;
    g.range = false;
    refused();
    g.range = true;
    a.feature = "@checkRange";
    refused();
    std::cout << "SDK adapter query, scope, and refusal checks passed\n";
}
