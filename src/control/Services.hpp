#pragma once
// Services.hpp — the taskPilot API surface
//
// Every operation taskPilot offers is declared here exactly once, as a
// MethodSpec (name + description + JSON-Schema for params). Two frontends
// derive from that single table:
//
//   1. JsonRpcServer builds its dispatch registry from it (TCP control socket).
//   2. McpServer builds its `tools/list` response from it (stdio MCP).
//
// pulseTrader keeps those two lists side by side by hand, which is a standing
// invitation for them to drift. Here the catalog is static data compiled into
// the binary, so the `mcp` subcommand can publish the tool list even when no
// daemon is running, and adding a method automatically exposes it over both
// transports.
//
// Method names are the MCP tool names verbatim — same rule as pulseTrader's
// control plane, for the same reason: one vocabulary everywhere.

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Clock.hpp"
#include "core/Result.hpp"
#include "core/TaskStore.hpp"

namespace taskpilot
{

/// One callable operation.
struct MethodSpec
{
    std::string name;            ///< Wire name, identical on both transports.
    std::string description;     ///< Shown to the LLM in tools/list.
    nlohmann::json params_schema; ///< JSON Schema (type: object) for params.
};

/// Result type of a call, matching JsonRpcServer's handler contract.
using RpcResult = Result<nlohmann::json>;
using MethodHandler = std::function<RpcResult(const nlohmann::json &params)>;
using MethodRegistry = std::unordered_map<std::string, MethodHandler>;

/// Binds the method catalog to a store and a clock.
///
/// The clock is injected rather than read inside each method so that "now" is
/// sampled once per call: sampling it per task would let a slow query rank two
/// tasks against different instants, which is exactly the kind of
/// non-reproducible ordering the total order in PriorityEngine exists to
/// prevent.
///
/// Thread-safety: invoke() is safe to call concurrently. It holds no mutable
/// state of its own; all mutation goes through TaskStore, which serializes.
class Services
{
  public:
    Services(TaskStore &store, const Clock &clock, std::string version);

    /// The complete method catalog, in a stable display order (introspection
    /// first, then task mutations, then queries, then synchronisation, then
    /// settings).
    ///
    /// Static because it is pure data: the `mcp` subcommand calls it without
    /// constructing a Services (it has no store — it talks to the daemon).
    [[nodiscard]] static const std::vector<MethodSpec> &methodSpecs();

    /// The spec for one method, or nullptr if the name is unknown.
    [[nodiscard]] static const MethodSpec *findSpec(const std::string &name);

    /// Dispatch one call. Unknown method -> kInvalidArgument (the caller
    /// distinguishes "no such method" from "bad arguments" via the message).
    [[nodiscard]] RpcResult invoke(const std::string &method,
                                  const nlohmann::json &params);

    /// Handlers bound to this instance, ready for JsonRpcServer.
    [[nodiscard]] MethodRegistry registry();

    /// Server start instant, for the uptime reported by get_status.
    [[nodiscard]] std::int64_t startTime() const { return m_startedAt; }

  private:
    /// Parameter readers. Each validates its input and returns a
    /// kInvalidArgument error naming the offending parameter, so a caller
    /// never has to guess which field was wrong.
    ///
    /// getRequired* fail when the key is absent; getOptional* return the
    /// fallback when absent but still reject a present-but-wrong-typed value
    /// (e.g. "importance": "high") rather than silently using the fallback.
    [[nodiscard]] static Result<std::string> requiredString(const nlohmann::json &params,
                                                            const char *key);
    [[nodiscard]] static Result<std::int64_t> requiredInt(const nlohmann::json &params,
                                                          const char *key);
    [[nodiscard]] Status optionalInt(const nlohmann::json &params, const char *key,
                                     std::optional<std::int64_t> &out);

    // --- introspection ---
    [[nodiscard]] RpcResult handleGetStatus(const nlohmann::json &params);
    [[nodiscard]] RpcResult handleDescribeMethods(const nlohmann::json &params);

    // --- task mutations ---
    [[nodiscard]] RpcResult handleAddTask(const nlohmann::json &params);
    [[nodiscard]] RpcResult handleUpdateTask(const nlohmann::json &params);
    [[nodiscard]] RpcResult handleCompleteTask(const nlohmann::json &params);
    [[nodiscard]] RpcResult handleReopenTask(const nlohmann::json &params);
    [[nodiscard]] RpcResult handleDeleteTask(const nlohmann::json &params);

    // --- queries ---
    [[nodiscard]] RpcResult handleGetQueue(const nlohmann::json &params);
    [[nodiscard]] RpcResult handleListTasks(const nlohmann::json &params);
    [[nodiscard]] RpcResult handleGetTask(const nlohmann::json &params);
    [[nodiscard]] RpcResult handleGetStats(const nlohmann::json &params);

    // --- synchronisation ---
    [[nodiscard]] RpcResult handleExportTasks(const nlohmann::json &params);
    [[nodiscard]] RpcResult handleImportTasks(const nlohmann::json &params);

    // --- settings ---
    [[nodiscard]] RpcResult handleGetWeights(const nlohmann::json &params);
    [[nodiscard]] RpcResult handleSetWeights(const nlohmann::json &params);

    TaskStore &m_store;
    const Clock &m_clock;
    std::string m_version;
    std::int64_t m_startedAt{ 0 };
};

/// Map an ErrorCode onto the JSON-RPC error code used on the wire.
///
/// Standard codes for protocol-level faults; the -32000..-32099 range is
/// reserved by JSON-RPC 2.0 for application errors, which is where the domain
/// failures live (an unknown task id is not a transport error).
[[nodiscard]] int rpcErrorCode(ErrorCode code);

} // namespace taskpilot
