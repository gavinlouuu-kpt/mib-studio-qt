#pragma once

// Execution provider selection (YOFO impl spec S1): MIB_EXECUTION_PROVIDER
//   pz                      PS result ring through /dev/mem (Linux on the PZ7035 PS)
//   replay:<file>[@<fps>]   replay a record stream (e.g. a `pzres capture` file);
//                           fps <= 0 or omitted = as fast as ingested
//   none | host             no provider: the host pipeline (or, with science on
//                           the PL, no results on the PS)
// Unset means "pz" in a MIB_PL_SCIENCE build (the PZ7035 PS image, #501: the
// instrument runs without the variable) and none elsewhere.

#include "backend/processing/IExecutionProvider.h"

#include <memory>
#include <string>

namespace backend::processing::pz {

// nullptr for none; `error` is set when the value is present but unusable.
std::unique_ptr<IExecutionProvider> makeExecutionProvider(const std::string& spec, std::string* error);
std::unique_ptr<IExecutionProvider> makeExecutionProviderFromEnv(std::string* error);
// The spec makeExecutionProviderFromEnv uses: `env` (MIB_EXECUTION_PROVIDER, null when unset)
// wins; otherwise "pz" when built for the PL (`plScienceBuild`), else "".
std::string executionProviderSpec(const char* env, bool plScienceBuild);

} // namespace backend::processing::pz
