#pragma once

// Execution provider selection (YOFO impl spec S1): MIB_EXECUTION_PROVIDER
//   pz                      PS result ring through /dev/mem (Linux on the PZ7035 PS)
//   replay:<file>[@<fps>]   replay a record stream (e.g. a `pzres capture` file);
//                           fps <= 0 or omitted = as fast as ingested
// Unset or "host" means none: the host pipeline (or, with science on the PL,
// no results on the PS).

#include "backend/processing/IExecutionProvider.h"

#include <memory>
#include <string>

namespace backend::processing::pz {

// nullptr for none; `error` is set when the value is present but unusable.
std::unique_ptr<IExecutionProvider> makeExecutionProvider(const std::string& spec, std::string* error);
std::unique_ptr<IExecutionProvider> makeExecutionProviderFromEnv(std::string* error);

} // namespace backend::processing::pz
