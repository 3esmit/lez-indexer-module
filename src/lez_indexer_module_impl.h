#pragma once

#include <cstdint>
#include <string>

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
    /// Current ingestion status as a compact JSON object so a UI can tell
    /// "catching up" from "failed": { state (starting/syncing/caught_up/error),
    /// indexedBlockId, lastError }. Empty string if the indexer isn't running.
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

private:
    // Storage dir handed to the FFI, either:
    // - the host's instance persistence path
    // - the process working directory (".") when the host didn't provision one
    std::string resolveStorageDir(const char* method) const;

    // IndexerServiceFFI* — opaque here (see class comment); cast in the .cpp.
    void* indexer_service_ffi = nullptr;
};
