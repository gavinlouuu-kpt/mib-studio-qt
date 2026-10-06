// StageService test rig (#464): a fake ZC300 behind a real SerialBusManager,
// a ZC300 driver with short timeouts, and a reference store that outlives a
// service instance so an "application restart" can be simulated.
// Header-only; include as "support/stage_rig.h".
#pragma once

#include "backend/services/SerialBus.h"
#include "backend/services/StageService.h"
#include "backend/stage/zc300/Zc300Stage.h"

#include "support/fake_zc300.h"
#include "support/tempdir.h"

#include <memory>

namespace mib::test {

// Forwards to a store shared across service instances.
class SharedStageReferenceStore final : public backend::services::IStageReferenceStore {
public:
    explicit SharedStageReferenceStore(std::shared_ptr<backend::services::MemoryStageReferenceStore> inner)
        : inner_(std::move(inner)) {}
    std::optional<backend::services::StageReferenceRecord> load() override { return inner_->load(); }
    bool save(const backend::services::StageReferenceRecord& r) override { return inner_->save(r); }
    bool clear() override { return inner_->clear(); }

private:
    std::shared_ptr<backend::services::MemoryStageReferenceStore> inner_;
};

struct StageRig {
    FakeZc300 device;
    backend::services::serialbus::SerialBusManager bus;
    std::shared_ptr<backend::services::MemoryStageReferenceStore> store =
        std::make_shared<backend::services::MemoryStageReferenceStore>();
    TempDir dir{"stage_rig"};
    // The supervised limit check has passed for the fake (serial 26017)
    // unless a test calls unverifyLimits().
    std::shared_ptr<backend::stage::LimitsVerificationStore> limits =
        std::make_shared<backend::stage::LimitsVerificationStore>((dir.path() / "limits.json").string());

    explicit StageRig(FakeZc300Config controller = FakeZc300Config{}) : device(controller)
    {
        bus.setSerialPortFactory([this] { return std::make_unique<FakeZc300Port>(device); });
        limits->save({"26017", "2026-10-06T00:00:00Z", -3000.0, 3000.0, 6000.0, "test rig"});
    }

    void unverifyLimits()
    {
        std::error_code ec;
        std::filesystem::remove(limits->path(), ec);
    }

    backend::services::StageConfig config() const
    {
        backend::services::StageConfig c;
        c.enabled = true;
        c.endpoint.systemPort = device.portName;
        c.pollMovingMs = 5;
        c.pollIdleMs = 20;
        return c;
    }

    std::unique_ptr<backend::services::StageService> service(const backend::services::StageConfig& c)
    {
        backend::stage::zc300::Zc300Stage::Timing timing;
        timing.transactionMs = 150;
        auto s = std::make_unique<backend::services::StageService>(
            [this, timing] { return std::make_unique<backend::stage::zc300::Zc300Stage>(bus, timing); },
            std::make_unique<SharedStageReferenceStore>(store), limits);
        s->setConfig(c);
        return s;
    }
    std::unique_ptr<backend::services::StageService> service() { return service(config()); }
    // A service over a caller-supplied zero store (fault-injecting or recording).
    std::unique_ptr<backend::services::StageService> serviceWithStore(
        const backend::services::StageConfig& c, std::unique_ptr<backend::services::IStageReferenceStore> zeroStore)
    {
        backend::stage::zc300::Zc300Stage::Timing timing;
        timing.transactionMs = 150;
        auto s = std::make_unique<backend::services::StageService>(
            [this, timing] { return std::make_unique<backend::stage::zc300::Zc300Stage>(bus, timing); },
            std::move(zeroStore), limits);
        s->setConfig(c);
        return s;
    }
};

} // namespace mib::test
