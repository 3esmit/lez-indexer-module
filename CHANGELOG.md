# Changelog

## 1.1.3 — 2026-09-17

- Published the indexer against the maintained Bedrock-compatible parser.
- Accepts both `body_root` and deployed `block_root` block-header fields.

## 1.1.0-alpha.1 — 2026-07-27

- Added the V1 managed-node lifecycle API: `nodeStatus()`,
  `nodeAction(QString)`, and `nodeChanged(QString)`.
- Added channel-scoped lifecycle snapshots, ordered action events, and
  compatibility with the existing Indexer API.
- Added Linux and macOS Apple Silicon package CI plus a prerelease workflow.
