#include "lez_indexer_module_impl.h"

#include "lez_ffi_marshalling.h"
#include <indexer_ffi.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace marshalling;

namespace {
    constexpr size_t MAX_NODE_LIFECYCLE_REQUEST_BYTES = 64 * 1024;
    constexpr size_t MAX_NODE_LIFECYCLE_CONFIG_PATH_BYTES = 4 * 1024;
    constexpr size_t MAX_NODE_LIFECYCLE_OPERATION_ID_BYTES = 128;
    constexpr size_t MAX_COMPLETED_NODE_LIFECYCLE_OPERATIONS = 64;
    constexpr const char* NODE_LIFECYCLE_SNAPSHOT_SCHEMA = "logos.managed_node_lifecycle.snapshot";
    constexpr const char* NODE_LIFECYCLE_COMMAND_SCHEMA = "logos.managed_node_lifecycle.command";
    constexpr const char* NODE_LIFECYCLE_ACK_SCHEMA = "logos.managed_node_lifecycle.ack";
    constexpr const char* NODE_LIFECYCLE_EVENT_SCHEMA = "logos.managed_node_lifecycle.event";
    std::atomic<std::uint64_t> node_lifecycle_instance_counter{0};

    std::int64_t nodeLifecycleTimestampMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()
        )
            .count();
    }

    std::string makeNodeLifecycleInstanceId() {
        return "indexer-" + std::to_string(nodeLifecycleTimestampMs()) + "-" +
               std::to_string(node_lifecycle_instance_counter.fetch_add(1) + 1);
    }

    bool containsEmbeddedNul(const std::string& value) {
        return value.find('\0') != std::string::npos;
    }

    bool isValidNodeLifecycleOperationId(const std::string& value) {
        if (value.empty() || value.size() > MAX_NODE_LIFECYCLE_OPERATION_ID_BYTES) {
            return false;
        }
        return std::all_of(value.begin(), value.end(), [](const unsigned char character) {
            return std::isalnum(character) || character == '.' || character == '_' || character == ':' ||
                   character == '-';
        });
    }

    bool parseLifecycleUnsigned(const nlohmann::json& value, std::uint64_t& output) {
        if (value.is_number_unsigned()) {
            output = value.get<std::uint64_t>();
            return true;
        }
        if (value.is_number_integer()) {
            const auto signed_value = value.get<std::int64_t>();
            if (signed_value >= 0) {
                output = static_cast<std::uint64_t>(signed_value);
                return true;
            }
        }
        return false;
    }

    bool isLifecycleVersionOne(const nlohmann::json& value) {
        std::uint64_t version = 0;
        return parseLifecycleUnsigned(value, version) && version == 1;
    }

    nlohmann::json nodeLifecycleError(const std::string& code, const std::string& message, const std::int64_t at_ms) {
        return {
            {"code", code},
            {"message", message},
            {"at_ms", at_ms},
        };
    }

    bool readIndexerChannelId(const std::string& config_path, std::string& channel_id) {
        if (config_path.empty() || config_path.size() > MAX_NODE_LIFECYCLE_CONFIG_PATH_BYTES ||
            containsEmbeddedNul(config_path) || !std::filesystem::path(config_path).is_absolute()) {
            return false;
        }
        try {
            std::ifstream in(config_path);
            if (!in) {
                return false;
            }
            const nlohmann::json config = nlohmann::json::parse(in);
            const auto found = config.find("channel_id");
            if (found == config.end() || !found->is_string()) {
                return false;
            }
            const std::string parsed = found->get<std::string>();
            if (!isHex(parsed, 64)) {
                return false;
            }
            channel_id = parsed;
            return true;
        } catch (const nlohmann::json::exception&) {
            return false;
        }
    }

    // The impl header keeps the handle opaque (void*) so the universal codegen
    // never needs the FFI types; recover the real type here.
    inline IndexerServiceFFI* handle(void* p) {
        return static_cast<IndexerServiceFFI*>(p);
    }

    // Module diagnostics go to stderr; logos_host captures it and routes each line
    // through its own logger, classifying by an "Error:"/"Warning:" token found
    // anywhere in the line (plain lines are treated as info). Prefix accordingly.
    void info(const char* method, const std::string& msg) {
        std::fprintf(stderr, "lez_indexer_module: %s: %s\n", method, msg.c_str());
    }
    void warn(const char* method, const std::string& msg) {
        std::fprintf(stderr, "lez_indexer_module: Warning: %s: %s\n", method, msg.c_str());
    }
    void error(const char* method, const std::string& msg) {
        std::fprintf(stderr, "lez_indexer_module: Error: %s: %s\n", method, msg.c_str());
    }
} // namespace

LezIndexerModuleImpl::LezIndexerModuleImpl()
    : lifecycleInstanceId(makeNodeLifecycleInstanceId()), lifecycleUpdatedAtMs(nodeLifecycleTimestampMs()) {}

LezIndexerModuleImpl::~LezIndexerModuleImpl() {
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex);
        ++lifecycleGeneration;
        lifecycleState = LifecycleState::Destroying;
        lifecyclePending = false;
        activeLifecycleOperationId.clear();
        activeLifecycleAction.clear();
        activeLifecycleGeneration = 0;
    }

    // A module teardown has no subscriber that can observe a lifecycle event.
    // Release the FFI handle directly rather than emitting into a dying context.
    if (stopIndexerPrepared() != 0) {
        error("destructor", "indexer FFI error on stop");
    }
}

// === Indexer Lifecycle ===

std::string LezIndexerModuleImpl::resolveStorageDir(const char* method) const {
    // The host owns where state lives (its instance persistence path). When that
    // isn't provisioned (e.g. running outside Basecamp), fall back to the process
    // working directory — the same "." the FFI's start_indexer uses — so start and
    // reset always agree on the store location.
    const std::string& path = instancePersistencePath();
    if (!path.empty()) {
        return path;
    }
    warn(method, "no instance persistence path; using the working directory");
    return ".";
}

int64_t LezIndexerModuleImpl::startIndexerPrepared(const std::string& config_path) {
    if (indexer_service_ffi) {
        info("start_indexer", "indexer already running; ignoring start request");
        return 0;
    }

    // Null runtime: the FFI creates and owns its own tokio runtime.
    const std::string storage = resolveStorageDir("start_indexer");
    info("start_indexer", "starting indexer");

    InitializedIndexerServiceFFIResult res = ::start_indexer(nullptr, config_path.c_str(), storage.c_str());
    if (is_error(&res.error)) {
        error(
            "start_indexer",
            "FFI failed to start indexer (OperationStatus " + std::to_string(static_cast<int64_t>(res.error)) + ")"
        );
        return static_cast<int64_t>(res.error);
    }

    indexer_service_ffi = res.value;
    info("start_indexer", "indexer started");
    return 0;
}

int64_t LezIndexerModuleImpl::stopIndexerPrepared() {
    if (!indexer_service_ffi) {
        return 0; // not running
    }
    // stop_indexer frees the handle; null ours before returning so a later start
    // (or the destructor) doesn't double-free or operate on a dead pointer.
    OperationStatus operation_result = ::stop_indexer(handle(indexer_service_ffi));
    indexer_service_ffi = nullptr;
    if (is_error(&operation_result)) {
        error(
            "stop_indexer",
            "FFI error on stop (OperationStatus " + std::to_string(static_cast<int64_t>(operation_result)) + ")"
        );
        return static_cast<int64_t>(operation_result);
    }
    info("stop_indexer", "indexer stopped");
    return 0;
}

int64_t LezIndexerModuleImpl::resetStoragePrepared(const std::string&, const std::string& channel_id) {
    // FFI stores RocksDB at <storage>/rocksdb-{channel_id}
    const std::filesystem::path storage = resolveStorageDir("reset_storage");
    // A channel id is a 32-byte hex string. Validate before building a path so a
    // malformed/edited config can't inject separators ("../", absolute paths) and
    // make remove_all escape the storage dir.
    if (!isHex(channel_id, 64)) {
        error("reset_storage", "indexer channel id is invalid; refusing to wipe storage");
        return -1;
    }

    const std::filesystem::path store = storage / ("rocksdb-" + channel_id);
    std::error_code ec;
    // remove_all returns the number of entries removed and only sets `ec` on a real
    // error; a missing store removes 0 with no error. No exists() pre-check — that
    // would mask an IO/permission error as a successful "nothing to wipe".
    const std::uintmax_t removed = std::filesystem::remove_all(store, ec);
    if (ec) {
        error("reset_storage", "failed to remove indexer storage");
        return -1;
    }
    info("reset_storage", removed == 0 ? "no indexer store to wipe" : "wiped indexer store");
    return 0;
}

const char* LezIndexerModuleImpl::lifecycleStateName(const LifecycleState state) {
    switch (state) {
    case LifecycleState::Uninitialized:
        return "uninitialized";
    case LifecycleState::Initializing:
        return "initializing";
    case LifecycleState::Stopped:
        return "stopped";
    case LifecycleState::Starting:
        return "starting";
    case LifecycleState::Running:
        return "running";
    case LifecycleState::Stopping:
        return "stopping";
    case LifecycleState::Destroying:
        return "destroying";
    }
    return "uninitialized";
}

std::vector<std::string> LezIndexerModuleImpl::lifecycleActions(const LifecycleState state) {
    switch (state) {
    case LifecycleState::Uninitialized:
        return {"start"};
    case LifecycleState::Stopped:
        return {"start", "reset"};
    case LifecycleState::Running:
        return {"stop"};
    case LifecycleState::Initializing:
    case LifecycleState::Starting:
    case LifecycleState::Stopping:
    case LifecycleState::Destroying:
        return {};
    }
    return {};
}

const char* LezIndexerModuleImpl::lifecycleFailureCode(const std::string& action) {
    if (action == "start")
        return "start_failed";
    if (action == "stop")
        return "stop_failed";
    if (action == "reset")
        return "reset_failed";
    return "lifecycle_action_failed";
}

const char* LezIndexerModuleImpl::lifecycleFailureMessage(const std::string& action) {
    if (action == "start")
        return "Indexer start failed.";
    if (action == "stop")
        return "Indexer stop failed.";
    if (action == "reset")
        return "Indexer reset failed.";
    return "Indexer lifecycle action failed.";
}

std::string LezIndexerModuleImpl::lifecycleSnapshotLocked() const {
    nlohmann::json snapshot;
    snapshot["schema"] = NODE_LIFECYCLE_SNAPSHOT_SCHEMA;
    snapshot["version"] = 1;
    snapshot["instance_id"] = lifecycleInstanceId;
    snapshot["epoch"] = lifecycleEpoch;
    snapshot["sequence"] = lifecycleSequence;
    snapshot["scope"] = {
        {"kind", "indexer"},
        {"channel_id", lifecycleChannelId.empty() ? nlohmann::json(nullptr) : nlohmann::json(lifecycleChannelId)},
    };
    snapshot["state"] = lifecycleStateName(lifecycleState);
    snapshot["health"] = !lifecycleError.empty()                     ? "degraded"
                         : lifecycleState == LifecycleState::Running ? "healthy"
                                                                     : "unknown";
    snapshot["supported_actions"] = lifecycleActions(lifecycleState);
    if (lifecyclePending) {
        snapshot["pending_operation"] = {
            {"operation_id",
             activeLifecycleOperationId.empty() ? nlohmann::json(nullptr) : nlohmann::json(activeLifecycleOperationId)},
            {"action", activeLifecycleAction},
        };
    } else {
        snapshot["pending_operation"] = nullptr;
    }

    const auto completed = lifecycleOperations.find(lastCompletedLifecycleOperationId);
    if (completed != lifecycleOperations.end() && completed->second.settled) {
        snapshot["last_completed_operation"] = {
            {"operation_id", lastCompletedLifecycleOperationId},
            {"action", completed->second.action},
            {"outcome", completed->second.outcome},
        };
    } else {
        snapshot["last_completed_operation"] = nullptr;
    }
    snapshot["last_error"] = lifecycleError.empty()
                                 ? nlohmann::json(nullptr)
                                 : nodeLifecycleError(lifecycleErrorCode, lifecycleError, lifecycleErrorAtMs);
    snapshot["updated_at_ms"] = lifecycleUpdatedAtMs;
    return snapshot.dump();
}

std::string LezIndexerModuleImpl::lifecycleEventLocked(
    const std::string& action,
    const std::string& operation_id,
    const std::string& phase,
    const std::string& outcome,
    const LifecycleState previous_state,
    const std::string& error_code
) const {
    nlohmann::json event;
    event["schema"] = NODE_LIFECYCLE_EVENT_SCHEMA;
    event["version"] = 1;
    event["instance_id"] = lifecycleInstanceId;
    event["epoch"] = lifecycleEpoch;
    event["sequence"] = lifecycleSequence;
    event["scope"] = {
        {"kind", "indexer"},
        {"channel_id", lifecycleChannelId.empty() ? nlohmann::json(nullptr) : nlohmann::json(lifecycleChannelId)},
    };
    event["operation_id"] = operation_id.empty() ? nlohmann::json(nullptr) : nlohmann::json(operation_id);
    event["action"] = action;
    event["phase"] = phase;
    event["outcome"] = outcome;
    event["previous_state"] = lifecycleStateName(previous_state);
    event["status"] = nlohmann::json::parse(lifecycleSnapshotLocked());
    event["error"] = error_code.empty()
                         ? nlohmann::json(nullptr)
                         : nodeLifecycleError(error_code, lifecycleFailureMessage(action), nodeLifecycleTimestampMs());
    event["emitted_at_ms"] = nodeLifecycleTimestampMs();
    return event.dump();
}

void LezIndexerModuleImpl::emitLifecycleEvents(const std::vector<std::string>& events) {
    for (const std::string& event : events) {
        nodeChanged(event);
    }
}

void LezIndexerModuleImpl::rememberCompletedLifecycleOperationLocked(const std::string& operation_id) {
    if (operation_id.empty()) {
        return;
    }
    lastCompletedLifecycleOperationId = operation_id;
    completedLifecycleOperationIds.push_back(operation_id);
    while (completedLifecycleOperationIds.size() > MAX_COMPLETED_NODE_LIFECYCLE_OPERATIONS) {
        const std::string expired = completedLifecycleOperationIds.front();
        completedLifecycleOperationIds.pop_front();
        const auto found = lifecycleOperations.find(expired);
        if (found != lifecycleOperations.end() && found->second.settled &&
            expired != lastCompletedLifecycleOperationId) {
            lifecycleOperations.erase(found);
        }
    }
}

LezIndexerModuleImpl::LifecycleDispatch LezIndexerModuleImpl::beginLifecycleAction(
    const std::string& action,
    const std::string& operation_id,
    const std::string& request_fingerprint,
    const bool has_expected_snapshot,
    const std::string& expected_instance_id,
    const std::uint64_t expected_epoch,
    const std::uint64_t expected_sequence,
    const bool strict_action
) {
    LifecycleDispatch dispatch;
    dispatch.action = action;
    dispatch.operationId = operation_id;

    std::lock_guard<std::mutex> lock(lifecycleMutex);
    const auto acknowledgement = [&](const bool accepted,
                                     const bool duplicate,
                                     const std::string& error_code,
                                     const std::string& error_message) {
        nlohmann::json result;
        result["schema"] = NODE_LIFECYCLE_ACK_SCHEMA;
        result["version"] = 1;
        result["operation_id"] = operation_id.empty() ? nlohmann::json(nullptr) : nlohmann::json(operation_id);
        result["accepted"] = accepted;
        result["duplicate"] = duplicate;
        result["instance_id"] = lifecycleInstanceId;
        result["epoch"] = lifecycleEpoch;
        result["sequence"] = lifecycleSequence;
        result["state"] = lifecycleStateName(lifecycleState);
        result["error"] = error_code.empty()
                              ? nlohmann::json(nullptr)
                              : nodeLifecycleError(error_code, error_message, nodeLifecycleTimestampMs());
        return result.dump();
    };
    const auto settle_without_dispatch = [&](const LifecycleDispatchDisposition disposition,
                                             const bool accepted,
                                             const std::string& outcome,
                                             const std::string& error_code,
                                             const std::string& error_message) {
        LifecycleOperation operation;
        operation.action = action;
        operation.requestFingerprint = request_fingerprint;
        operation.previousState = lifecycleState;
        operation.settled = true;
        operation.outcome = outcome;
        const auto inserted = lifecycleOperations.emplace(operation_id, std::move(operation));
        if (accepted) {
            ++lifecycleSequence;
            lifecycleUpdatedAtMs = nodeLifecycleTimestampMs();
            dispatch.events.push_back(
                lifecycleEventLocked(action, operation_id, "accepted", "accepted", lifecycleState)
            );
        }
        ++lifecycleSequence;
        lifecycleUpdatedAtMs = nodeLifecycleTimestampMs();
        dispatch.disposition = disposition;
        dispatch.events.push_back(lifecycleEventLocked(
            action, operation_id, "settled", outcome, lifecycleState, accepted ? std::string() : error_code
        ));
        dispatch.acknowledgement = acknowledgement(
            accepted, false, accepted ? std::string() : error_code, accepted ? std::string() : error_message
        );
        inserted.first->second.acknowledgement = dispatch.acknowledgement;
        rememberCompletedLifecycleOperationLocked(operation_id);
    };

    if (strict_action) {
        const auto existing = lifecycleOperations.find(operation_id);
        if (existing != lifecycleOperations.end()) {
            if (existing->second.requestFingerprint != request_fingerprint) {
                dispatch.disposition = LifecycleDispatchDisposition::Rejected;
                dispatch.acknowledgement = acknowledgement(
                    false, false, "operation_id_conflict", "operation_id was already used for a different request."
                );
                return dispatch;
            }
            dispatch.disposition = LifecycleDispatchDisposition::Duplicate;
            nlohmann::json duplicate = nlohmann::json::parse(existing->second.acknowledgement);
            duplicate["duplicate"] = true;
            dispatch.acknowledgement = duplicate.dump();
            return dispatch;
        }
    }
    if (lifecyclePending) {
        if (strict_action) {
            settle_without_dispatch(
                LifecycleDispatchDisposition::Rejected,
                false,
                "rejected",
                "operation_in_progress",
                "A lifecycle operation is already in progress."
            );
        }
        return dispatch;
    }
    if (strict_action && has_expected_snapshot &&
        (expected_instance_id != lifecycleInstanceId || expected_epoch != lifecycleEpoch ||
         expected_sequence != lifecycleSequence)) {
        settle_without_dispatch(
            LifecycleDispatchDisposition::Rejected,
            false,
            "rejected",
            "state_mismatch",
            "The lifecycle snapshot is stale."
        );
        return dispatch;
    }
    if (strict_action) {
        if (action == "start") {
            if (lifecycleState == LifecycleState::Running) {
                settle_without_dispatch(LifecycleDispatchDisposition::Noop, true, "no_op", {}, {});
                return dispatch;
            }
            if (lifecycleState != LifecycleState::Uninitialized && lifecycleState != LifecycleState::Stopped) {
                settle_without_dispatch(
                    LifecycleDispatchDisposition::Rejected,
                    false,
                    "rejected",
                    "invalid_state",
                    "Indexer is not in a startable state."
                );
                return dispatch;
            }
        } else if (action == "stop") {
            if (lifecycleState == LifecycleState::Stopped) {
                settle_without_dispatch(LifecycleDispatchDisposition::Noop, true, "no_op", {}, {});
                return dispatch;
            }
            if (lifecycleState != LifecycleState::Running) {
                settle_without_dispatch(
                    LifecycleDispatchDisposition::Rejected,
                    false,
                    "rejected",
                    "invalid_state",
                    "Indexer is not in a stoppable state."
                );
                return dispatch;
            }
        } else if (action == "reset") {
            if (lifecycleState != LifecycleState::Stopped) {
                settle_without_dispatch(
                    LifecycleDispatchDisposition::Rejected,
                    false,
                    "rejected",
                    "invalid_state",
                    "Stop the Indexer before resetting its storage."
                );
                return dispatch;
            }
        }
    }

    dispatch.previousState = lifecycleState;
    dispatch.generation = ++lifecycleGeneration;
    lifecycleState = action == "start"  ? LifecycleState::Starting
                     : action == "stop" ? LifecycleState::Stopping
                                        : LifecycleState::Destroying;
    lifecycleError.clear();
    lifecycleErrorCode.clear();
    lifecycleErrorAtMs = 0;
    lifecyclePending = true;
    activeLifecycleOperationId = strict_action ? operation_id : std::string();
    activeLifecycleAction = action;
    activeLifecycleGeneration = dispatch.generation;
    ++lifecycleSequence;
    lifecycleUpdatedAtMs = nodeLifecycleTimestampMs();
    dispatch.disposition = LifecycleDispatchDisposition::Dispatch;
    if (strict_action) {
        LifecycleOperation operation;
        operation.action = action;
        operation.requestFingerprint = request_fingerprint;
        operation.previousState = dispatch.previousState;
        const auto inserted = lifecycleOperations.emplace(operation_id, std::move(operation));
        dispatch.events.push_back(
            lifecycleEventLocked(action, operation_id, "accepted", "accepted", dispatch.previousState)
        );
        dispatch.acknowledgement = acknowledgement(true, false, {}, {});
        inserted.first->second.acknowledgement = dispatch.acknowledgement;
    } else {
        dispatch.events.push_back(lifecycleEventLocked(action, {}, "accepted", "accepted", dispatch.previousState));
    }
    return dispatch;
}

void LezIndexerModuleImpl::settleLifecycleAction(
    const LifecycleDispatch& dispatch,
    const bool success,
    const LifecycleState success_state,
    const LifecycleState failure_state,
    const std::string& error_code
) {
    std::string event;
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex);
        if (!lifecyclePending || activeLifecycleGeneration != dispatch.generation ||
            activeLifecycleAction != dispatch.action) {
            return;
        }
        lifecycleState = success ? success_state : failure_state;
        lifecycleErrorCode = success              ? std::string()
                             : error_code.empty() ? lifecycleFailureCode(dispatch.action)
                                                  : error_code;
        lifecycleError = success ? std::string() : lifecycleFailureMessage(dispatch.action);
        lifecycleErrorAtMs = success ? 0 : nodeLifecycleTimestampMs();
        if (success && (dispatch.action == "reset" ||
                        (dispatch.action == "start" && dispatch.previousState == LifecycleState::Uninitialized))) {
            ++lifecycleEpoch;
        }
        if (!dispatch.operationId.empty()) {
            const auto operation = lifecycleOperations.find(dispatch.operationId);
            if (operation != lifecycleOperations.end()) {
                operation->second.settled = true;
                operation->second.outcome = success ? "succeeded" : "failed";
                rememberCompletedLifecycleOperationLocked(dispatch.operationId);
            }
        }
        lifecyclePending = false;
        activeLifecycleOperationId.clear();
        activeLifecycleAction.clear();
        activeLifecycleGeneration = 0;
        ++lifecycleSequence;
        lifecycleUpdatedAtMs = nodeLifecycleTimestampMs();
        event = lifecycleEventLocked(
            dispatch.action,
            dispatch.operationId,
            "settled",
            success ? "succeeded" : "failed",
            dispatch.previousState,
            success ? std::string() : lifecycleErrorCode
        );
    }
    nodeChanged(event);
}

int64_t LezIndexerModuleImpl::start_indexer(const std::string& config_path) {
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex);
        if (!lifecyclePending && indexer_service_ffi && lifecycleState == LifecycleState::Running) {
            return 0;
        }
    }

    const LifecycleDispatch dispatch = beginLifecycleAction("start", {}, {}, false, {}, 0, 0, false);
    if (dispatch.disposition != LifecycleDispatchDisposition::Dispatch) {
        return -1;
    }
    emitLifecycleEvents(dispatch.events);
    const int64_t started = startIndexerPrepared(config_path);
    if (started == 0) {
        std::string channel_id;
        if (readIndexerChannelId(config_path, channel_id)) {
            std::lock_guard<std::mutex> lock(lifecycleMutex);
            if (activeLifecycleGeneration == dispatch.generation) {
                lifecycleConfigPath = config_path;
                lifecycleChannelId = channel_id;
            }
        }
    }
    settleLifecycleAction(dispatch, started == 0, LifecycleState::Running, dispatch.previousState);
    return started;
}

int64_t LezIndexerModuleImpl::stop_indexer() {
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex);
        if (!lifecyclePending && !indexer_service_ffi &&
            (lifecycleState == LifecycleState::Uninitialized || lifecycleState == LifecycleState::Stopped)) {
            return 0;
        }
    }

    const LifecycleDispatch dispatch = beginLifecycleAction("stop", {}, {}, false, {}, 0, 0, false);
    if (dispatch.disposition != LifecycleDispatchDisposition::Dispatch) {
        return -1;
    }
    emitLifecycleEvents(dispatch.events);
    const int64_t stopped = stopIndexerPrepared();
    // The FFI releases its handle even when it returns an error status, so the
    // observed state is stopped with degraded health instead of falsely running.
    settleLifecycleAction(dispatch, stopped == 0, LifecycleState::Stopped, LifecycleState::Stopped);
    return stopped;
}

int64_t LezIndexerModuleImpl::reset_storage(const std::string& config_path) {
    // Preserve the legacy operation: stop first so RocksDB closes before wipe.
    const int64_t stop_code = stop_indexer();
    if (stop_code != 0) {
        error("reset_storage", "could not stop indexer before wiping storage");
        return stop_code;
    }

    std::string channel_id;
    if (!readIndexerChannelId(config_path, channel_id)) {
        error("reset_storage", "could not read a valid indexer channel id from configuration");
        return -1;
    }

    const LifecycleDispatch dispatch = beginLifecycleAction("reset", {}, {}, false, {}, 0, 0, false);
    if (dispatch.disposition != LifecycleDispatchDisposition::Dispatch) {
        return -1;
    }
    emitLifecycleEvents(dispatch.events);
    const int64_t reset = resetStoragePrepared(config_path, channel_id);
    if (reset == 0) {
        std::lock_guard<std::mutex> lock(lifecycleMutex);
        if (activeLifecycleGeneration == dispatch.generation) {
            lifecycleConfigPath = config_path;
            lifecycleChannelId = channel_id;
        }
    }
    settleLifecycleAction(dispatch, reset == 0, LifecycleState::Stopped, LifecycleState::Stopped);
    return reset;
}

std::string LezIndexerModuleImpl::nodeStatus() {
    std::lock_guard<std::mutex> lock(lifecycleMutex);
    return lifecycleSnapshotLocked();
}

std::string LezIndexerModuleImpl::nodeAction(const std::string& request) {
    const auto rejected = [this](const std::string& code, const std::string& message) {
        std::lock_guard<std::mutex> lock(lifecycleMutex);
        nlohmann::json acknowledgement;
        acknowledgement["schema"] = NODE_LIFECYCLE_ACK_SCHEMA;
        acknowledgement["version"] = 1;
        acknowledgement["operation_id"] = nullptr;
        acknowledgement["accepted"] = false;
        acknowledgement["duplicate"] = false;
        acknowledgement["instance_id"] = lifecycleInstanceId;
        acknowledgement["epoch"] = lifecycleEpoch;
        acknowledgement["sequence"] = lifecycleSequence;
        acknowledgement["state"] = lifecycleStateName(lifecycleState);
        acknowledgement["error"] = nodeLifecycleError(code, message, nodeLifecycleTimestampMs());
        return acknowledgement.dump();
    };

    if (request.size() > MAX_NODE_LIFECYCLE_REQUEST_BYTES) {
        return rejected("request_too_large", "Lifecycle request exceeds the supported size.");
    }
    nlohmann::json input;
    try {
        input = nlohmann::json::parse(request);
    } catch (const nlohmann::json::exception&) {
        return rejected("invalid_request", "Lifecycle request must be a JSON object.");
    }
    if (!input.is_object()) {
        return rejected("invalid_request", "Lifecycle request must be a JSON object.");
    }
    for (const auto& item : input.items()) {
        const std::string& key = item.key();
        if (key != "schema" && key != "version" && key != "operation_id" && key != "action" && key != "expected" &&
            key != "parameters") {
            return rejected("invalid_request", "Lifecycle request contains an unsupported field.");
        }
    }
    const auto schema = input.find("schema");
    if (schema == input.end() || !schema->is_string() || schema->get<std::string>() != NODE_LIFECYCLE_COMMAND_SCHEMA) {
        return rejected("invalid_request", "Unsupported lifecycle request schema.");
    }
    const auto version = input.find("version");
    if (version == input.end() || !isLifecycleVersionOne(*version)) {
        return rejected("invalid_request", "Unsupported lifecycle request version.");
    }
    const auto operation_id = input.find("operation_id");
    if (operation_id == input.end() || !operation_id->is_string()) {
        return rejected("invalid_request", "Lifecycle request requires an operation_id.");
    }
    const std::string operation = operation_id->get<std::string>();
    if (!isValidNodeLifecycleOperationId(operation)) {
        return rejected("invalid_request", "Lifecycle operation_id is invalid.");
    }
    const auto action_value = input.find("action");
    if (action_value == input.end() || !action_value->is_string()) {
        return rejected("invalid_request", "Lifecycle request requires an action.");
    }
    const std::string action = action_value->get<std::string>();
    if (action != "start" && action != "stop" && action != "reset") {
        return rejected("invalid_request", "Unsupported lifecycle action.");
    }

    bool has_expected_snapshot = false;
    std::string expected_instance_id;
    std::uint64_t expected_epoch = 0;
    std::uint64_t expected_sequence = 0;
    const auto expected = input.find("expected");
    if (expected != input.end()) {
        if (!expected->is_object() || expected->size() != 3 || !expected->contains("instance_id") ||
            !expected->contains("epoch") || !expected->contains("sequence") ||
            !expected->at("instance_id").is_string() ||
            !parseLifecycleUnsigned(expected->at("epoch"), expected_epoch) ||
            !parseLifecycleUnsigned(expected->at("sequence"), expected_sequence)) {
            return rejected(
                "invalid_request", "Lifecycle expected snapshot must contain instance_id, epoch, and sequence."
            );
        }
        expected_instance_id = expected->at("instance_id").get<std::string>();
        has_expected_snapshot = true;
    }

    nlohmann::json parameters = nlohmann::json::object();
    const auto parameters_value = input.find("parameters");
    if (parameters_value != input.end()) {
        if (!parameters_value->is_object()) {
            return rejected("invalid_request", "Lifecycle parameters must be an object.");
        }
        parameters = *parameters_value;
    }

    std::string config_path;
    std::string channel_id;
    const auto select_config = [&](const bool required) {
        if (parameters.empty()) {
            std::lock_guard<std::mutex> lock(lifecycleMutex);
            config_path = lifecycleConfigPath;
            channel_id = lifecycleChannelId;
            if (!required && config_path.empty()) {
                return true;
            }
            return !config_path.empty() && readIndexerChannelId(config_path, channel_id);
        }
        if (parameters.size() != 1 || !parameters.contains("config_path") ||
            !parameters.at("config_path").is_string()) {
            return false;
        }
        config_path = parameters.at("config_path").get<std::string>();
        return readIndexerChannelId(config_path, channel_id);
    };

    if (action == "start") {
        if (!select_config(true)) {
            return rejected("invalid_request", "Start requires a valid absolute parameters.config_path.");
        }
    } else if (action == "reset") {
        if (!select_config(true)) {
            return rejected("invalid_request", "Reset requires a valid absolute parameters.config_path.");
        }
    } else if (!parameters.empty()) {
        return rejected("invalid_request", "Stop does not accept parameters.");
    }

    const LifecycleDispatch dispatch = beginLifecycleAction(
        action,
        operation,
        input.dump(),
        has_expected_snapshot,
        expected_instance_id,
        expected_epoch,
        expected_sequence,
        true
    );
    emitLifecycleEvents(dispatch.events);
    if (dispatch.disposition != LifecycleDispatchDisposition::Dispatch) {
        return dispatch.acknowledgement;
    }

    if (action == "start") {
        const int64_t started = startIndexerPrepared(config_path);
        if (started == 0) {
            std::lock_guard<std::mutex> lock(lifecycleMutex);
            if (activeLifecycleGeneration == dispatch.generation) {
                lifecycleConfigPath = config_path;
                lifecycleChannelId = channel_id;
            }
        }
        settleLifecycleAction(dispatch, started == 0, LifecycleState::Running, dispatch.previousState);
    } else if (action == "stop") {
        const int64_t stopped = stopIndexerPrepared();
        settleLifecycleAction(dispatch, stopped == 0, LifecycleState::Stopped, LifecycleState::Stopped);
    } else {
        const int64_t reset = resetStoragePrepared(config_path, channel_id);
        if (reset == 0) {
            std::lock_guard<std::mutex> lock(lifecycleMutex);
            if (activeLifecycleGeneration == dispatch.generation) {
                lifecycleConfigPath = config_path;
                lifecycleChannelId = channel_id;
            }
        }
        settleLifecycleAction(dispatch, reset == 0, LifecycleState::Stopped, LifecycleState::Stopped);
    }
    return dispatch.acknowledgement;
}

// === Indexer Queries ===
//
// Each method calls the matching query_* FFI function on the handle we hold,
// marshals the returned C struct into compact JSON, then frees the FFI
// allocation with the matching free_ffi_* function. The frees take the outer
// pointer (PointerResult.value) and reclaim the whole allocation.

std::string LezIndexerModuleImpl::getAccount(const std::string& account_id) {
    if (!indexer_service_ffi) {
        warn("getAccount", "indexer not started");
        return {};
    }

    FfiAccountId id;
    if (!accountStrToBytes32(account_id, &id)) {
        warn("getAccount", "invalid account id (need Base58 or 32-byte hex)");
        return {};
    }

    PointerResult_FfiAccount__OperationStatus res = ::query_account(handle(indexer_service_ffi), id);
    if (is_error(&res.error) || !res.value) {
        warn("getAccount", "indexer FFI error");
        return {};
    }

    const std::string out = jsonToCompactString(ffiAccountToJson(*res.value));
    ::free_ffi_account(res.value);
    return out;
}

std::string LezIndexerModuleImpl::getBlockById(const std::string& block_id) {
    if (!indexer_service_ffi) {
        warn("getBlockById", "indexer not started");
        return {};
    }

    char* end = nullptr;
    const uint64_t id = std::strtoull(block_id.c_str(), &end, 10);
    if (end == block_id.c_str() || *end != '\0') {
        warn("getBlockById", "invalid block id");
        return {};
    }

    PointerResult_FfiBlockOpt__OperationStatus res = ::query_block(handle(indexer_service_ffi), id);
    if (is_error(&res.error) || !res.value) {
        warn("getBlockById", "indexer FFI error");
        return {};
    }

    std::string out;
    if (res.value->is_some && res.value->value) {
        out = jsonToCompactString(ffiBlockToJson(*res.value->value));
    }
    ::free_ffi_block_opt(res.value);
    return out;
}

std::string LezIndexerModuleImpl::getBlockByHash(const std::string& hash) {
    if (!indexer_service_ffi) {
        warn("getBlockByHash", "indexer not started");
        return {};
    }

    FfiHashType h;
    if (!hexToBytes32(hash, &h)) {
        warn("getBlockByHash", "invalid hash (need 32-byte hex)");
        return {};
    }

    PointerResult_FfiBlockOpt__OperationStatus res = ::query_block_by_hash(handle(indexer_service_ffi), h);
    if (is_error(&res.error) || !res.value) {
        warn("getBlockByHash", "indexer FFI error");
        return {};
    }

    std::string out;
    if (res.value->is_some && res.value->value) {
        out = jsonToCompactString(ffiBlockToJson(*res.value->value));
    }
    ::free_ffi_block_opt(res.value);
    return out;
}

std::string LezIndexerModuleImpl::getTransaction(const std::string& hash) {
    if (!indexer_service_ffi) {
        warn("getTransaction", "indexer not started");
        return {};
    }

    FfiHashType h;
    if (!hexToBytes32(hash, &h)) {
        warn("getTransaction", "invalid hash (need 32-byte hex)");
        return {};
    }

    PointerResult_FfiOption_FfiTransaction_____OperationStatus res =
        ::query_transaction(handle(indexer_service_ffi), h);
    if (is_error(&res.error) || !res.value) {
        warn("getTransaction", "indexer FFI error");
        return {};
    }

    std::string out;
    if (res.value->is_some && res.value->value) {
        out = jsonToCompactString(ffiTransactionToJson(*res.value->value));
    }
    ::free_ffi_transaction_opt(res.value);
    return out;
}

std::string LezIndexerModuleImpl::getBlocks(const std::string& before, const std::string& limit) {
    if (!indexer_service_ffi) {
        warn("getBlocks", "indexer not started");
        return {};
    }

    char* end = nullptr;
    const uint64_t limitNum = std::strtoull(limit.c_str(), &end, 10);
    if (end == limit.c_str() || *end != '\0') {
        warn("getBlocks", "invalid limit");
        return {};
    }

    // `before` is the optional pagination cursor: an empty string means "from
    // the tip", a non-empty value that fails to parse is an error. `beforeVal`
    // must outlive the call since FfiOption_u64 borrows its address.
    uint64_t beforeVal = 0;
    bool hasBefore = false;
    if (!before.empty()) {
        char* bend = nullptr;
        beforeVal = std::strtoull(before.c_str(), &bend, 10);
        if (bend == before.c_str() || *bend != '\0') {
            warn("getBlocks", "invalid before");
            return {};
        }
        hasBefore = true;
    }
    FfiOption_u64 beforeOpt;
    beforeOpt.is_some = hasBefore;
    beforeOpt.value = hasBefore ? &beforeVal : nullptr;

    PointerResult_FfiVec_FfiBlock_____OperationStatus res =
        ::query_block_vec(handle(indexer_service_ffi), beforeOpt, limitNum);
    if (is_error(&res.error) || !res.value) {
        warn("getBlocks", "indexer FFI error");
        return {};
    }

    nlohmann::json arr = nlohmann::json::array();
    for (uintptr_t i = 0; i < res.value->len; ++i) {
        arr.push_back(ffiBlockToJson(res.value->entries[i]));
    }
    ::free_ffi_block_vec(res.value);
    return jsonToCompactString(arr);
}

std::string LezIndexerModuleImpl::getLastFinalizedBlockId() {
    if (!indexer_service_ffi) {
        warn("getLastFinalizedBlockId", "indexer not started");
        return {};
    }

    LastBlockIdResult res = ::query_last_block(handle(indexer_service_ffi));
    if (is_error(&res.error) || !res.is_some) {
        warn("getLastFinalizedBlockId", "indexer FFI error or no finalized block yet");
        return {};
    }

    // Bare decimal string; the id is returned inline, nothing to free.
    return u64ToString(res.block_id);
}

std::string LezIndexerModuleImpl::getStatus() {
    if (!indexer_service_ffi) {
        warn("getStatus", "indexer not started");
        return {};
    }

    // The FFI builds the status JSON itself (schema owned by indexer_core), so
    // there is nothing to marshal — copy the C string out and free it.
    char* json = ::query_status(handle(indexer_service_ffi));
    if (!json) {
        warn("getStatus", "indexer FFI error");
        return {};
    }

    const std::string out(json);
    ::free_cstring(json);
    return out;
}

std::string LezIndexerModuleImpl::getTransactionsByAccount(
    const std::string& account_id,
    const std::string& offset,
    const std::string& limit
) {
    if (!indexer_service_ffi) {
        warn("getTransactionsByAccount", "indexer not started");
        return {};
    }

    FfiAccountId id;
    if (!accountStrToBytes32(account_id, &id)) {
        warn("getTransactionsByAccount", "invalid account id (need Base58 or 32-byte hex)");
        return {};
    }

    char* offEnd = nullptr;
    char* limEnd = nullptr;
    const uint64_t offsetNum = std::strtoull(offset.c_str(), &offEnd, 10);
    const uint64_t limitNum = std::strtoull(limit.c_str(), &limEnd, 10);
    if (offEnd == offset.c_str() || *offEnd != '\0' || limEnd == limit.c_str() || *limEnd != '\0') {
        warn("getTransactionsByAccount", "invalid offset/limit");
        return {};
    }

    PointerResult_FfiVec_FfiTransaction_____OperationStatus res =
        ::query_transactions_by_account(handle(indexer_service_ffi), id, offsetNum, limitNum);
    if (is_error(&res.error) || !res.value) {
        warn("getTransactionsByAccount", "indexer FFI error");
        return {};
    }

    nlohmann::json arr = nlohmann::json::array();
    for (uintptr_t i = 0; i < res.value->len; ++i) {
        arr.push_back(ffiTransactionToJson(res.value->entries[i]));
    }
    ::free_ffi_transaction_vec(res.value);
    return jsonToCompactString(arr);
}
