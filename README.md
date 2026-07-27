# Logos Execution Zone Indexer Module

A Logos Core **service module** (`type: core`, universal authoring model) that runs the Logos Execution Zone (L2) indexer and exposes it to the Logos ecosystem. It is a thin Qt-free plugin around the `indexer_ffi` library from
[`logos-execution-zone`](https://github.com/logos-blockchain/logos-execution-zone): it starts the indexer, which connects to an L1/bedrock node and indexes the zone's channel, and exposes **query methods over the Logos protocol** (Qt Remote Objects).

Registered module name: **`lez_indexer_module`**. Its public methods _are_ its API — other modules call them in-process over the Logos protocol. It pairs with the
[`lez-explorer-ui`](https://github.com/logos-co/lez-explorer-ui) block explorer, which reads from it **in-process over the Logos protocol** (typed `modules().lez_indexer_module.*` calls) — no RPC endpoint or socket in between.

> [!TIP]
>
> **Keep the module name short.** `logos_host` derives a Qt Remote Objects `local:` socket from it (`local:logos_<name>_<id>`), and Unix socket path names are limited to at most 108 bytes (see [man unix](https://man7.org/linux/man-pages/man7/unix.7.html)). The build emits the library with no `lib` prefix so its filename equals the registered name (`lez_indexer_module`) — consumers derive the QtRO invoke target from the filename, so the two must stay identical.

## Setup

### IDE

If you're using an IDE with CMake integration make sure it points to the same cmake directory as the `justfile`, which defaults to `build`.

This will reduce friction when working on the project.

### Nix

- Use `nix flake update` to bring all nix context and packages
- Use `nix build` to build the package (produces `lez_indexer_module.<dylib|so|dll>`)
- Use `nix build .#unit-tests` to build and run the lifecycle contract tests
- Use `nix run` to launch the module-viewer and check your module loads properly
- Use `nix develop` to setup your IDE

## Package releases

This repository owns portable LEZ Indexer package releases. Run **Publish LEZ
Indexer Module** from `main` after updating `metadata.json` and adding a
matching `## [version]` entry to `CHANGELOG.md`.

The release workflow builds and requires both `linux-amd64` and
`darwin-arm64`, verifies that each package manifest matches `metadata.json`,
merges them into one LGX, writes a SHA-256 sidecar, and publishes an alpha
GitHub release tagged `lez_indexer_module-v<version>`. Releases are unsigned
until a signing key policy is introduced.

Validate the same source-release contract locally with:

```bash
bash scripts/test-source-release-workflow.sh
nix build .#lgx-portable --out-link result-lgx -L
bash scripts/verify-portable-package.sh result-lgx/*.lgx linux-amd64
```

## Usage

The module does **not** start the indexer on load — something must invoke its `start_indexer` method (via the module-viewer's invoke panel, or another module / basecamp over the Logos protocol):

```c
start_indexer(config_path)
```

- `config_path` — **absolute** path to a JSON config (see
  [`config/indexer_config.json`](config/indexer_config.json)). It must be absolute: the module runs inside the `logos_host` subprocess, whose working directory is not your shell's.

On success it returns `0`; a non-zero return is the FFI `OperationStatus` (e.g. `2 = InitializationError`). Once started, consume the indexer through the query methods below.

> [!TIP]
>
> The module logs its lifecycle and `OperationStatus` failures to stderr, which
> `logos_host` captures and surfaces through its own logger. For ongoing indexer health — sync
> state, and a parked/stalled tip with its reason — poll `getStatus()`; that runs
> inside the indexer and reports what the C++ lifecycle logs can't see.

### Managed node lifecycle V1

Hosts that manage nodes should prefer the standard V1 surface instead of
inferring lifecycle state from query failures:

| Method | Purpose |
| --- | --- |
| `nodeStatus()` | Returns a versioned lifecycle snapshot. `scope.channel_id` is `null` before a valid configuration has been accepted, then identifies the indexed zone. |
| `nodeAction(request)` | Accepts a caller-correlated V1 JSON command and immediately returns an acknowledgement. |
| `nodeChanged(event)` | Emits ordered V1 accepted and settled observations for each accepted command. |

The command envelope is `logos.managed_node_lifecycle.command`, version `1`.
It requires an `operation_id` and one of these actions:

- `start` — from `uninitialized` or `stopped`. The first start requires
  `parameters.config_path`; later starts may reuse the last accepted config.
- `stop` — from `running`.
- `reset` — from `stopped`; removes the current channel's RocksDB store and
  leaves the Indexer stopped.

`parameters.config_path`, when supplied, must be an absolute path to a valid
Indexer JSON config with a 64-character hexadecimal `channel_id`. Lifecycle
snapshots, acknowledgements, and events never include that path or config
contents. Existing `start_indexer`, `stop_indexer`, and `reset_storage` methods
remain compatible; they also update the V1 snapshot and emit uncorrelated
events.

### Query methods (the Logos-protocol API)

Each returns a compact JSON string (an **empty** string means not-found / failed query); all ids/hashes are 32-byte hex, all numeric args/ids are decimal strings.

| Method                                                | Returns                                                                              |
| ----------------------------------------------------- | ------------------------------------------------------------------------------------ |
| `getStatus()`                                         | indexer status JSON (schema owned by `indexer_core`)                                 |
| `getLastFinalizedBlockId()`                           | tip block id (bare decimal string)                                                   |
| `getBlockById(block_id)`                              | block JSON                                                                           |
| `getBlockByHash(hash)`                                | block JSON                                                                           |
| `getBlocks(before, limit)`                            | JSON array of blocks; `before` = `""` for the tip, else a block id to page back from |
| `getTransaction(hash)`                                | transaction JSON                                                                     |
| `getAccount(account_id)`                              | account JSON (the payload omits the id; callers inject the queried id)               |
| `getTransactionsByAccount(account_id, offset, limit)` | JSON array of transactions touching the account                                      |

Because the module uses the universal authoring model (`interface: "universal"`), it publishes a typed LIDL contract, so universal consumers get Qt-typed wrappers (`modules().lez_indexer_module.getBlockById(...)`) rather than dynamic by-name calls.

### Configuration

`config/indexer_config.json` is deserialized into the indexer's `IndexerConfig`. Key fields:

| Field                 | Meaning                                                                         | Default                 |
| --------------------- | ------------------------------------------------------------------------------- | ----------------------- |
| `bedrock_config.addr` | L1/bedrock node URL the indexer reads from; it must be reachable.               | `http://localhost:8080` |
| `channel_id`          | The zone channel the indexer consumes; must match what the sequencer inscribes. |                         |

The config keys must match the `IndexerConfig` schema of the `logos-execution-zone` rev pinned in `flake.nix`/`flake.lock`; bumping that rev may require re-syncing this file. Unknown keys are ignored.
