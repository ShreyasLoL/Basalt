# Phase 4 persistence implementation

## Backend selection

The configured Windows/Clang toolchain does not provide SQLite (`cmake --find-package -DNAME=SQLite3 ...` reports not found). Phase 4 therefore uses the roadmap's deterministic transactional file-store fallback. Catalog metadata and history are authoritative files on disk; the persistence library has no in-memory-only mode. The SQLite schema in `DATA_MODEL.md` remains the logical target as a SQLite package becomes available.

## Catalog and migrations

`Catalog` writes a deterministic JSON document with sorted dataset IDs, schema version 1, and a migration record containing the migration name and SHA-256 of its definition. Updates write and flush a sibling temporary file, then atomically replace the catalog. A version 0 catalog is migrated by adding the version 1 metadata defaults for license, interval, instrument and range. Unknown future schema versions and invalid migration checksums fail closed. Reopening reads the persisted catalog rather than reconstructing it from process memory.

The file backend currently persists the dataset catalog. Instruments and trading entities use their domain repositories when those phases are implemented; this file is not a claim that all SQLite tables in the logical schema already exist.

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

The writer requires `recover()` before appending, flushes each append to the file, rejects non-increasing sequences and oversized payloads, and never rewrites earlier frames. One shared `EventLog` instance serializes concurrent appends; `append_next` allocates its monotonic sequence under the same lock. Recovery scans to the first invalid, truncated, oversized, or checksum-failing frame. It copies that suffix to the first available `.corrupt[.N]` sibling, durably truncates the log to the last valid frame, and reports the accepted prefix and quarantined byte count. Reads independently recheck frame metadata and CRCs.

## Checkpoints and deterministic replay

Checkpoint files use `BCP1`, event sequence, bounded state-hash and state-blob lengths, CRC-32 and payload, and are atomically replaced. The state hash is provided by the owning domain's canonical state encoder. `replay_from_checkpoint` verifies the snapshot hash, applies only later frames in strict sequence order, and returns state and replay manifest hashes. The event SHA-256 input is the concatenation of each replayed sequence (u64 little-endian), payload length (u64 little-endian), and payload. Replaying identical snapshot and frames with the same callbacks produces identical state and manifest hashes.

## Historical bars and datasets

CSV imports use integer columns `open_time_ns,open_micro,high_micro,low_micro,close_micro,volume`; timestamps must increase, OHLC values must be positive and valid, and import size is limited to 256 MiB. Values are canonicalized before storage. Dataset metadata records instrument, interval, source, license tag, range, import timestamp, relative filename and SHA-256 of the canonical persisted CSV. Reads verify the file hash before parsing; exports use an atomic replacement and preserve the canonical integer representation.

All durable paths return typed errors on open, write, flush, rename, schema, corruption, bounds, or hash failures. No market-data or trading values are converted to floating point by persistence.
