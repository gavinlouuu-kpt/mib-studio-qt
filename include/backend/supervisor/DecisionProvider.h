// AI Experiment Supervisor — provider-neutral decision interface (issue #422).
//
// JEV, deterministic rule providers, scripted test providers and future
// LLM-based supervisors all implement this. A provider receives a frozen
// snapshot and the policy limits, and returns a DecisionResult. It has no
// access to the backend, no hardware handle and no way to execute anything.
//
// Threading: evaluate() is invoked on the supervisor worker thread (or the
// harness thread) — never on acquisition, processing, trigger or recording
// threads. It must be bounded: honor policy.providerTimeoutMs and return a
// Timeout error instead of hanging. cancel() may be called from another
// thread to ask an in-flight evaluate() to return early; the default is a
// no-op for providers that are already bounded.
#pragma once

#include "backend/supervisor/DecisionContract.h"
#include "backend/supervisor/ExperimentSnapshot.h"

#include <string>

namespace backend::supervisor {

class DecisionProvider {
public:
    virtual ~DecisionProvider() = default;

    virtual std::string name() const = 0;
    virtual std::string version() const = 0;
    // Pinned model/version identity, empty for deterministic providers.
    virtual std::string modelVersion() const { return {}; }

    virtual DecisionResult evaluate(const ExperimentSnapshot& snapshot,
                                    const DecisionPolicy& policy) = 0;

    virtual void cancel() {}
    // Called by the service before it (re)starts its worker so a cancel that
    // landed between evaluations does not abort the next run.
    virtual void resetCancel() {}
};

} // namespace backend::supervisor
