// RAII observer that delivers DeviceDiscoveryService snapshots to a QObject
// on its own thread (issue #419, ADR 0005).
//
// The service invokes observers on a discovery worker thread; this helper
// re-posts each snapshot to `receiver` with a queued invocation so widgets
// only ever see results on the UI thread. Destruction removes the observer
// and blocks until an in-flight invocation has returned, so a subscription
// owned as a member of the receiving QObject makes the receiver safe to
// destroy at any time: queued calls that have not run yet are dropped by Qt
// together with the receiver.
#pragma once

#include "backend/discovery/DeviceDiscoveryService.h"

#include <QMetaObject>
#include <QObject>

#include <cstdint>
#include <functional>
#include <utility>

namespace frontend {

class DiscoverySubscription {
public:
    using Handler = std::function<void(const backend::discovery::DiscoverySnapshot&)>;

    DiscoverySubscription() = default;
    DiscoverySubscription(backend::discovery::DeviceDiscoveryService& service, QObject* receiver,
                          Handler handler)
    {
        subscribe(service, receiver, std::move(handler));
    }
    ~DiscoverySubscription() { reset(); }

    DiscoverySubscription(const DiscoverySubscription&) = delete;
    DiscoverySubscription& operator=(const DiscoverySubscription&) = delete;

    // (Re)subscribe. `receiver` must outlive this object or call reset() in
    // its destructor body — owning the subscription as a member is enough.
    void subscribe(backend::discovery::DeviceDiscoveryService& service, QObject* receiver,
                   Handler handler)
    {
        reset();
        service_ = &service;
        id_ = service.addObserver(
            [receiver, handler = std::move(handler)](const backend::discovery::DiscoverySnapshot& s) {
                // `receiver` is alive here: removal blocks until this returns.
                QMetaObject::invokeMethod(
                    receiver, [handler, s] { handler(s); }, Qt::QueuedConnection);
            });
    }

    void reset()
    {
        if (service_ && id_ != 0) service_->removeObserver(id_);
        service_ = nullptr;
        id_ = 0;
    }

    bool active() const { return id_ != 0; }

private:
    backend::discovery::DeviceDiscoveryService* service_ = nullptr;
    std::uint64_t id_ = 0;
};

} // namespace frontend
