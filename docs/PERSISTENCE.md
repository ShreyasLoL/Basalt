# Persistence implementation

## Backend selection

The configured Windows/Clang toolchain does not provide SQLite (`cmake --find-package -DNAME=SQLite3 ...` reports not found), and the build has no SQLite dependency. The active backend is the deterministic file-store design: dataset catalog metadata is JSON, historical bars are canonical CSV files with SHA-256 verification, and selected event journals use CRC-framed `.blog` logs. There is no in-memory-only catalog mode. The relational entities in `DATA_MODEL.md` describe a logical target, not active SQLite tables; ADR-009 records why SQLite is deferred and when to revisit it.

## Catalog and migrations

`Catalog` writes a deterministic JSON document with sorted dataset IDs, schema version 1, and a migration record containing the migration name and SHA-256 of its definition. Single and batch upserts validate first, read the catalog once, merge through a dataset-ID hash index, sort once for deterministic output, and atomically replace the file once. The merge is O(existing records + batch records), plus the final O(N log N) sort and single JSON rewrite; bulk history imports should use `upsert_datasets` instead of repeated single upserts. A version 0 catalog is migrated by adding the version 1 metadata defaults for license, interval, instrument and range. Unknown future schema versions and invalid migration checksums fail closed. Reopening reads the persisted catalog rather than reconstructing it from process memory.

Persistence is implemented by domain: catalog metadata and historical bars use JSON/CSV files, while selected trading, risk and kill-switch state uses event-log journals. The active formats and in-memory boundaries are enumerated in `DATA_MODEL.md`; this is not a complete relational persistence layer. ADR-009 and ADR-010 explain why SQLite is deferred and what evidence would change the decision.

## `.blog` event frames

Each frame is little-endian and consists of a 22-byte header followed by opaque UTF-8 or binary payload bytes:

| Offset | Width | Field |
| --- | ---: | --- |
| 0 | 4 | ASCII magic `BLG1` |
| 4 | 2 | Frame version (`1`) |
| 6 | 4 | Payload length (max 16 MiB) |
| 10 | 8 | Strictly increasing event sequence |
| 18 | 4 | CRC-32/ISO-HDLC over version, length, sequence and payload |
| 22 | length | Payload |

The writer requires `recover()` before appending and keeps one append handle open until `close()` or another `recover()`. Each append flushes the C++ stream and then durably flushes the file through the OS before it reports success; non-increasing sequences and oversized payloads are rejected, and earlier frames are never rewritten. If write or durable flush fails, the handle closes and the instance requires recovery before another append. `close()` durably flushes and closes the handle. `recover()` closes any prior handle, repairs an invalid suffix, and reopens the log, so callers can use it after an operator-controlled rotation. Rotation is not automatic and multi-file replay is not provided: rotate only after `close()`, archive that generation, and treat a new empty active file as a new sequence generation. One shared `EventLog` instance serializes concurrent appends; `append_next` allocates its monotonic sequence under the same lock. Recovery scans to the first invalid, truncated, oversized, or checksum-failing frame. It copies that suffix to the first available `.corrupt[.N]` sibling, durably truncates the log to the last valid frame, and reports the accepted prefix and quarantined byte count. Reads independently recheck frame metadata and CRCs.

Kill-switch frames from both the MCP adapter and trading persistence use the same versioned JSON event schema. Durable reasons are always `[REDACTED]`. Actor identifiers are retained only when they are 1–64 ASCII letters, digits, underscore, dash, dot or colon; other actor values are `[REDACTED]`. Restore also redacts legacy free-text reasons in memory. This does not rewrite pre-existing append-only frames; operators should protect or retire logs written before this policy if those frames may contain sensitive reasons.

## Checkpoints and deterministic replay

Checkpoint files use `BCP1`, event sequence, bounded state-hash and state-blob lengths, CRC-32 and payload, and are atomically replaced. The state hash is provided by the owning domain's canonical state encoder. `replay_from_checkpoint` verifies the snapshot hash, applies only later frames in strict sequence order, and returns state and replay manifest hashes. The event SHA-256 input is the concatenation of each replayed sequence (u64 little-endian), payload length (u64 little-endian), and payload. Replaying identical snapshot and frames with the same callbacks produces identical state and manifest hashes.

## Historical bars and datasets

CSV imports use integer columns `open_time_ns,open_micro,high_micro,low_micro,close_micro,volume`; timestamps must increase, OHLC values must be positive and valid, and import size is limited to 256 MiB. Values are canonicalized before storage. Dataset metadata records instrument, interval, source, license tag, range, import timestamp, relative filename and SHA-256 of the canonical persisted CSV. Reads verify the file hash before parsing from the same in-memory buffer without a stream copy, and `HistoryStore::sha256` streams in 64-byte blocks without allocating a padded copy of the input; exports use an atomic replacement and preserve the canonical integer representation.

All durable paths return typed errors on open, write, flush, rename, schema, corruption, bounds, or hash failures. No market-data or trading values are converted to floating point by persistence.
