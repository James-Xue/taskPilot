#pragma once
// RpcTypes.hpp — the reply type shared by the control layer and its clients
//
// `RpcResult` used to live in Services.hpp. That header also pulls in
// Clock.hpp and TaskStore.hpp, so a client that only needed the error type
// inherited the store and the SQLite header through it. Keeping the alias in
// its own self-contained header lets ControlClient (and the Qt UI that links
// it) stay independent of the storage layer.
//
// The header includes nlohmann/json.hpp directly because the alias names
// Result<nlohmann::json>; Result.hpp alone does not declare json.

#include <nlohmann/json.hpp>

#include "core/Result.hpp"

namespace taskpilot
{

/// Result type of a control-socket call.
using RpcResult = Result<nlohmann::json>;

} // namespace taskpilot
