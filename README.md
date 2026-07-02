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
- Use `nix run` to launch the module-viewer and check your module loads properly
- Use `nix develop` to setup your IDE

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
> The module logs its lifecycle (start/stop, the resolved config + storage paths,
> and failures with their `OperationStatus`) to stderr, which the `logos_host`
> captures and surfaces through its own logger. For ongoing indexer health — sync
> state, and a parked/stalled tip with its reason — poll `getStatus()`; that runs
> inside the indexer and reports what the C++ lifecycle logs can't see.

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
