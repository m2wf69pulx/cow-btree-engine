# cow-btree-engine

A copy-on-write B-tree storage engine in C99, built to study one question:
what does it actually take to survive a crash at any instruction?

Keys and values are `uint32`, pages are 4096 bytes, and every write path
allocates new pages instead of touching reachable ones. The engine is small
enough to read in one sitting and the test harness loses pages on purpose.

## Durability model

Commit order, in `bt_commit`:

1. `pwrite` all new pages (a put rewrites every node on its root path).
2. `fsync` the data file.
3. Write the next generation's root into the other superblock slot.
4. `fsync` again.

The two superblock slots alternate by generation, so the slot being written
is never the only path to the tree. Each page carries a CRC32 over its header
and cells; `bt_open` loads the newest generation whose tree verifies, then
falls back to the older slot when the newest one is torn. Pages past the last
reachable page come from interrupted commits and are truncated away on the
next writable open.

Invariants the code keeps:

- A page reachable from a committed root is never modified.
- A commit's data pages are durable before its superblock is written.
- Internal separators are the minimum key of the right child, so an equal key
  is routed right and never duplicated.
- Every page read is checksum-verified; verify also enforces ordering, page
  bounds and depth.

## Build and test

```sh
make            # builds build/cowbt
make check      # selftest plus 6 fork-based crash rounds
```

`make check` covers: insert/lookup/overwrite, sorted iteration and range
seeking, cache hits on repeated reads, invisibility of uncommitted puts after
reopen, recovery from a deliberately torn page, and repeated truncation-based
crashes.

## Usage

```sh
./build/cowbt init demo.db
./build/cowbt put demo.db 42 7
./build/cowbt get demo.db 42
./build/cowbt scan demo.db 40 5
./build/cowbt verify demo.db
./build/cowbt stats demo.db
./build/cowbt crash crash.db 8 4
```

Exit codes: `0` success, `1` missing key or usage error, `2` failure.

## Layout

- `include/cowbt.h` - page kinds, public API, invariants.
- `src/page.c` - page encoding, CRC32, positional I/O, page allocation.
- `src/cache/cache.c` - direct-mapped cache of verified pages, hit counters.
- `src/tree.c` - COW insert, lookup, cursor, superblock recovery, verify.
- `src/main.c` - CLI, self tests, crash harness.

## Read path cache

`bt_get` and the cursor consult a 64-slot direct-mapped cache of pages that
already passed CRC verification. It is safe without invalidation because a
reachable page is never rewritten: writes allocate new page ids and the old
ids keep their bytes. Misses fall back to `pread`, `verify` always reads from
the file, and `stats` reports hit/miss counters.

## Concurrency

One writer at a time via `flock(LOCK_EX)`; readers take `LOCK_SH`. A put is
visible to other processes only after `bt_commit`.

## Limits

Pages are never reused, so the file grows with write volume; a freelist is
the intended fix. There is no delete yet, no MVCC, and one key update rewrites
the whole root path. Single-file, single-writer, 32-bit keys and values.
