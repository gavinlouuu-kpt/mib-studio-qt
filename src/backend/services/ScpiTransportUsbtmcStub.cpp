// USBTMC transport placeholder for platforms with neither NI-VISA (Windows)
// nor the kernel usbtmc class driver (Linux): constructs, then fails open()
// with a message naming the gap. Keeps the factory total.
#include "backend/services/ScpiTransport.h"

namespace backend::services::scpi {

namespace {
class UnavailableUsbtmcTransport final : public IScpiTransport {
public:
    bool open(const std::string&, std::string* error) override {
        if (error) *error = "USBTMC is not supported on this platform; use the LAN transport";
        return false;
    }
    void close() override {}
    bool isOpen() const override { return false; }
    bool write(std::string_view, std::string* error) override {
        if (error) *error = "not open";
        return false;
    }
    bool readLine(std::string&, std::chrono::milliseconds, std::string* error) override {
        if (error) *error = "not open";
        return false;
    }
    std::string describe() const override { return "usb (unavailable)"; }
};
} // namespace

std::unique_ptr<IScpiTransport> makeUsbtmcScpiTransport() {
    return std::make_unique<UnavailableUsbtmcTransport>();
}

} // namespace backend::services::scpi
