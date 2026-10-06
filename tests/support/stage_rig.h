// StageService test rig (#464): a fake ZC300 behind a real SerialBusManager,
// a ZC300 driver with short timeouts, and a reference store that outlives a
// service instance so an "application restart" can be simulated.
// Header-only; include as "support/stage_rig.h".
#pragma once

#include "backend/services/SerialBus.h"
#include "backend/services/StageService.h"
#include "backend/stage/zc300/Zc300Stage.h"

#include "support/fake_zc300.h"

#include <memory>

namespace mib::test {

// Forwards to a store shared across service instances.
class SharedStageReferenceStore final : public backend::services::IStageReferenceStore {
public:
    explicit SharedStageReferenceStore(std::shared_ptr<backend::services::MemoryStageReferenceStore> inner)
        : inner_(std::move(inner)) {}
    std::optional<backend::services::StageReferenceRecord> load() override { return inner_->load(); }
    void save(const backend::services::StageReferenceRecord& r) override { inner_->save(r); }
    void clear() override { inner_->clear(); }

private:
    std::shared_ptr<backend::services::MemoryStageReferenceStore> inner_;
};

struct StageRig {
    FakeZc300 device;
    backend::services::serialbus::SerialBusManager bus;
    std::shared_ptr<backend::services::MemoryStageReferenceStore> store =
        std::make_shared<backend::services::MemoryStageReferenceStore>();

    explicit StageRig(FakeZc300Config controller = FakeZc300Config{}) : device(controller)
    {
        bus.setSerialPortFactory([this] { return std::make_unique<FakeZc300Port>(device); });
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
            std::make_unique<SharedStageReferenceStore>(store));
        s->setConfig(c);
        return s;
    }
    std::unique_ptr<backend::services::StageService> service() { return service(config()); }
};

} // namespace mib::test
