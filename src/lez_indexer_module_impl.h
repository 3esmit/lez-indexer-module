#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "logos_module_context.h"

/**
 * @brief Logos Execution Zone indexer module — wraps the LEZ indexer Rust FFI.
 *
 * Universal authoring model: this impl class is the whole module. Its public
 * methods ARE the API — callable by other modules (e.g. the LEZ explorer UI)
 * over the Logos protocol and from the CLI (`logoscore -c`). The Qt plugin glue
 * (the *Plugin / *Interface classes, Q_PLUGIN_METADATA, initLogos wiring) is
 * generated from this header by logos-module-builder.
 *
 * Module code is Qt-free (std::string, not QString). The FFI handle is held as
 * an opaque void* so this header stays free of the generated `indexer_ffi.h` —
 * the universal codegen parses this header, and we don't want it to depend on
 * the FFI types. The .cpp casts the handle back to `IndexerServiceFFI*`.
 *
 * Every query returns a compact JSON string; an EMPTY string means
 * not-found / failed query.
 */
class LezIndexerModuleImpl : public LogosModuleContext {
public:
    LezIndexerModuleImpl();
    ~LezIndexerModuleImpl();

    /// Boot ingestion against the indexer config at `config_path` (must be an
    /// ABSOLUTE path — the module runs in a logos_host subprocess). RocksDB state
    /// is stored under this module's instance persistence path (host-owned),
    /// independent of the config. Idempotent: a second call while already running
    /// is a no-op. Returns 0 on success, else the FFI OperationStatus code.
    /// int64_t (not int): the universal codegen marshals int64_t/std::string/bool
    /// as scalar wire types; a plain `int` return is treated as a JSON payload.
    int64_t start_indexer(const std::string& config_path);

    /// Stop ingestion and release the FFI handle. No-op (returns 0) when the
    /// indexer isn't running. Pair with start_indexer to apply a new config:
    /// stop, then start (start_indexer stays idempotent and won't restart on its
    /// own). Returns 0 on success, else the FFI OperationStatus code.
    int64_t stop_indexer();

    /// Stop the indexer (if running) and delete the RocksDB store for the config's
    /// channel — `<storage>/rocksdb-<channel_id>`, where `<storage>` is resolved the
    /// same way start_indexer resolves it (the host's instance persistence path, or
    /// the process working directory when unset) — so the next start_indexer
    /// re-indexes from scratch. The recovery path when the store is stale against a
    /// different/reset chain. Pass the same `config_path` given to start_indexer;
    /// does NOT restart. Returns 0 on success, else non-zero. int64_t for the same
    /// codegen reason as start_indexer.
    int64_t reset_storage(const std::string& config_path);

    /// Return the bounded, versioned lifecycle snapshot for this Indexer
    /// instance. Configuration paths and other host-local details are never
    /// included in this public result.
    std::string nodeStatus();

    /// Accept a versioned, caller-correlated Indexer lifecycle command.
    ///
    /// The request uses the shared managed-node V1 JSON envelope. It returns a
    /// small acknowledgement; accepted and settled observations are emitted via
    /// nodeChanged(). Existing start_indexer(), stop_indexer(), and
    /// reset_storage() methods remain compatible.
    std::string nodeAction(const std::string& request);

    /// Account by id, accepting Base58 (canonical) or 32-byte hex. The returned
    /// JSON omits the id; callers inject the queried id themselves.
    std::string getAccount(const std::string& account_id);
    /// Block by decimal block id.
    std::string getBlockById(const std::string& block_id);
    /// Block by 32-byte hex hash.
    std::string getBlockByHash(const std::string& hash);
    /// Transaction by 32-byte hex hash.
    std::string getTransaction(const std::string& hash);
    /// Page of blocks: `before` = "" for the tip, else a block id to page back
    /// from; `limit` is the decimal max count.
    std::string getBlocks(const std::string& before, const std::string& limit);
    /// Tip block id as a bare decimal string.
    std::string getLastFinalizedBlockId();
    /// Current ingestion status as the core's `IndexerStatus` JSON, passed
    /// through verbatim: { state (Starting/Syncing/CaughtUp/Error/Stalled),
    /// last_error, indexed_block_id, stall_reason }. Empty string if the
    /// indexer isn't running.
    std::string getStatus();
    /// Transactions touching `account_id` (Base58 or 32-byte hex), paginated by
    /// decimal `offset`/`limit`.
    //
    // MUST stay on a single line: the universal `--header-to-lidl` parser drops
    // methods whose parameter list spans multiple lines, so a wrapped signature
    // silently vanishes from the published LIDL contract (and thus from typed
    // consumers like lez_explorer_ui). clang-format off keeps the linter from
    // re-wrapping this long line.
    // clang-format off
    std::string getTransactionsByAccount(const std::string& account_id, const std::string& offset, const std::string& limit);
    // clang-format on

    // clang-format off
logos_events:
    /// Emits ordered, versioned lifecycle observations for nodeStatus() and
    /// nodeAction().
    void nodeChanged(const std::string& event);
    // clang-format on

private:
    enum class LifecycleState : std::uint8_t {
        Uninitialized,
        Initializing,
        Stopped,
        Starting,
        Running,
        Stopping,
        Destroying,
    };

    enum class LifecycleDispatchDisposition : std::uint8_t {
        Dispatch,
        Duplicate,
        Rejected,
        Noop,
    };

    struct LifecycleOperation {
        std::string action;
        std::string requestFingerprint;
        std::string acknowledgement;
        LifecycleState previousState = LifecycleState::Uninitialized;
        bool settled = false;
        std::string outcome;
    };

    struct LifecycleDispatch {
        LifecycleDispatchDisposition disposition = LifecycleDispatchDisposition::Rejected;
        std::string action;
        std::string operationId;
        LifecycleState previousState = LifecycleState::Uninitialized;
        std::uint64_t generation = 0;
        std::string acknowledgement;
        std::vector<std::string> events;
    };

    // Storage dir handed to the FFI, either:
    // - the host's instance persistence path
    // - the process working directory (".") when the host didn't provision one
    std::string resolveStorageDir(const char* method) const;

    int64_t startIndexerPrepared(const std::string& configPath);
    int64_t stopIndexerPrepared();
    int64_t resetStoragePrepared(const std::string& configPath, const std::string& channelId);

    LifecycleDispatch beginLifecycleAction(
        const std::string& action,
        const std::string& operationId,
        const std::string& requestFingerprint,
        bool hasExpectedSnapshot,
        const std::string& expectedInstanceId,
        std::uint64_t expectedEpoch,
        std::uint64_t expectedSequence,
        bool strictAction
    );
    void settleLifecycleAction(
        const LifecycleDispatch& dispatch,
        bool success,
        LifecycleState successState,
        LifecycleState failureState,
        const std::string& errorCode = {}
    );
    [[nodiscard]] std::string lifecycleSnapshotLocked() const;
    [[nodiscard]] std::string lifecycleEventLocked(
        const std::string& action,
        const std::string& operationId,
        const std::string& phase,
        const std::string& outcome,
        LifecycleState previousState,
        const std::string& errorCode = {}
    ) const;
    void emitLifecycleEvents(const std::vector<std::string>& events);
    void rememberCompletedLifecycleOperationLocked(const std::string& operationId);
    [[nodiscard]] static const char* lifecycleStateName(LifecycleState state);
    [[nodiscard]] static std::vector<std::string> lifecycleActions(LifecycleState state);
    [[nodiscard]] static const char* lifecycleFailureCode(const std::string& action);
    [[nodiscard]] static const char* lifecycleFailureMessage(const std::string& action);

    // IndexerServiceFFI* — opaque here (see class comment); cast in the .cpp.
    void* indexer_service_ffi = nullptr;

    mutable std::mutex lifecycleMutex;
    LifecycleState lifecycleState = LifecycleState::Uninitialized;
    std::uint64_t lifecycleGeneration = 0;
    std::string lifecycleInstanceId;
    std::uint64_t lifecycleEpoch = 0;
    std::uint64_t lifecycleSequence = 0;
    std::int64_t lifecycleUpdatedAtMs = 0;
    std::int64_t lifecycleErrorAtMs = 0;
    std::string lifecycleErrorCode;
    std::string lifecycleError;
    bool lifecyclePending = false;
    std::string activeLifecycleOperationId;
    std::string activeLifecycleAction;
    std::uint64_t activeLifecycleGeneration = 0;
    std::string lastCompletedLifecycleOperationId;
    std::string lifecycleConfigPath;
    std::string lifecycleChannelId;
    std::unordered_map<std::string, LifecycleOperation> lifecycleOperations;
    std::deque<std::string> completedLifecycleOperationIds;
};
