# cpp-tsdb

Single-node time-series engine in C++20. No third-party libraries.

It keeps a write-ahead log, a tag inverted index, and sealed segments with delta-of-delta timestamps and XOR-encoded values. Reopen replays the WAL, so a crash between flushes does not drop acknowledged writes.

## Layout

- `include/tsdb/types.hpp` series keys, queries, aggregates
- `src/codec.cpp` Gorilla-style block codec
- `src/engine.cpp` catalog, tag index, WAL, segment flush, recovery, query
- `src/main.cpp` CLI

## Build

```bash
cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

## CLI

```bash
./build/tsdb-cli write ./data cpu 1710000000000 0.42 host=a dc=sjc
./build/tsdb-cli query ./data cpu 0 2000000000000 avg host=a
./build/tsdb-cli flush ./data
./build/tsdb-cli stats ./data
```

Aggregates: `raw`, `sum`, `avg`, `min`, `max`, `count`. Tag filters are equality matches and are intersected. Same timestamp on a series overwrites the previous value.

MIT
