#include "backend/camera/features/EGrabberFeaturePort.h"
#include <cstdlib>
#include <iostream>
#include <typeindex>

using namespace camera::features;
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
    std::vector<std::string> lists; // every list query string seen, in order
    template <class M> std::string getString(const std::string& key) {
        // Tooltip fallback: only the SDK-example spelling answers.
        if (key == std::string(Euresys::query::info("NodeB", "Tooltip"))) return "tip-b";
        if (key == std::string(Euresys::query::info("NodeB", "DisplayName"))) return "Node B";
        if (key.rfind("@info", 0) == 0) throw std::runtime_error("absent metadata");
        return {};
    }
    template <class M> std::vector<std::string> getStringList(const std::string& key) {
        module = typeid(M);
        lists.push_back(key);
        if (key == std::string(Euresys::query::categories(false))) return {"CatRoot", "CatChild"};
        if (key == std::string(Euresys::query::categoriesOf("CatRoot", false))) return {"CatChild"};
        if (key == std::string(Euresys::query::featuresOf("CatChild", true))) return {"NodeB"};
        if (key == std::string(Euresys::query::features(false))) return {"NodeB", "NodeC"};
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
    // E-1 enumeration: the adapter composes exactly the SDK's query strings,
    // dispatches by module, and returns the grabber's names untouched.
    g.lists.clear();
    check(port.categories(Module::Stream, false) ==
          std::vector<std::string>{"CatRoot", "CatChild"});
    check(g.module == typeid(Euresys::StreamModule));
    check(port.categoriesOf(Module::Device, "CatRoot", false) ==
          std::vector<std::string>{"CatChild"});
    check(g.module == typeid(Euresys::DeviceModule));
    check(port.featuresOf(Module::Remote, "CatChild", true) == std::vector<std::string>{"NodeB"});
    check(port.features(Module::Interface, false) ==
          std::vector<std::string>{"NodeB", "NodeC"});
    check(g.module == typeid(Euresys::InterfaceModule));
    check(g.lists.size() == 4 && g.lists[0] == "@categories" &&
          g.lists[1] == "@categories CatRoot" && g.lists[2] == "@!features CatChild" &&
          g.lists[3] == "@features");
    const auto info = port.info(Module::Remote, "NodeB");
    check(info.displayName == "Node B" && info.toolTip == "tip-b" && info.description.empty() &&
          info.visibility.empty());
    check(port.canEnumerate());
    // Names are spliced into query strings: query syntax is refused before the SDK.
    const auto listsBefore = g.lists.size();
    for (const char* bad : {"CatRoot =~ .*", "A B", "", "@features"}) {
        bool threw = false;
        try {
            port.featuresOf(Module::Remote, bad, false);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        check(threw);
    }
    check(g.lists.size() == listsBefore);
    std::cout << "SDK adapter query, scope, and refusal checks passed\n";
    return 0;
}
