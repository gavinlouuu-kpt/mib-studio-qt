#include "backend/pz/AlignLock.h"

#include <cstdio>

namespace backend::pz {

namespace {

std::string describe(const IngressStatus& st) {
    char text[160];
    std::snprintf(text, sizeof text, "ingress status 0x%08X (lane overflow 0x%02X), errors %u, resyncs %u", st.status,
                  st.laneOverflow(), st.errors, st.resyncs);
    return text;
}

} // namespace

AlignLockResult awaitAlignLock(const AlignLockHooks& hooks, const AlignLockPolicy& policy) {
    AlignLockResult out;
    if (hooks.waitPreview(policy.firstPreviewWait)) {
        out.locked = true;
        return out;
    }
    IngressStatus st;
    const bool haveStatus = hooks.readStatus && hooks.readStatus(st);
    if (!haveStatus || st.laneOverflow() == 0) {
        // Not the stuck signature: the plain slow start gets the rest of the wait.
        if (hooks.waitPreview(policy.slowStartWait)) {
            out.locked = true;
            return out;
        }
        out.error = "no Align preview arrived" + (haveStatus ? " (" + describe(st) + ")" : std::string());
        return out;
    }
    for (int attempt = 0; attempt < policy.attempts; ++attempt) {
        if (hooks.onAttempt) hooks.onAttempt(attempt + 1, st);
        ++out.clears;
        out.recovered = true;
        if (!hooks.clearFlags(policy.receiverResetHold)) {
            out.error = "Align preview not locking: the receiver reset was refused";
            return out;
        }
        hooks.pause(policy.settleAfterClear);
        if (hooks.waitPreview(policy.previewWaitAfterClear)) {
            out.locked = true;
            return out;
        }
        hooks.readStatus(st);
    }
    out.error = "Align preview not locking: " + describe(st) + " after " + std::to_string(out.clears) +
                " receiver resets (" + std::to_string(policy.receiverResetHold.count()) + " ms each)";
    return out;
}

} // namespace backend::pz
