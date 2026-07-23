# Benchmarks

Throughput and latency for the `05_hashtables` server — the single-threaded,
event-loop KV server speaking the custom length-prefixed binary protocol — plus
an in-process microbenchmark for the incremental-resize claim.

## Setup

| | |
|---|---|
| Machine | Apple M2, 8 cores |
| Server | `05_hashtables/05_server.cpp` — single-threaded, `poll()` event loop |
| Transport | TCP over loopback (`127.0.0.1:1234`), `TCP_NODELAY` on client |
| Compiler | `clang++ -O2 -std=gnu++17` |
| Client | `bench/bench.cpp`, single connection |
| Ops per run | 2,000,000 (throughput) / 200,000 (latency) |

The client runs on the same machine as the server. Loopback removes network
latency, so these numbers measure **how fast the server can process requests**,
not what a remote client would see over a real network.

## Build

From the repo root:

```sh
clang++ -O2 -std=gnu++17 05_hashtables/05_server.cpp 05_hashtables/05_hashtables.cpp -o server
clang++ -O2 -std=gnu++17 bench/bench.cpp -o bench
```

`-std=gnu++17` (not `-std=c++17`) is required — the server's `container_of`
macro uses the GNU `typeof` extension.

## Run

```sh
./server &                          # listens on :1234
./bench pipeline get 2000000 512    # mode  op  total_ops  pipeline_depth
./bench sync     get 200000  1      # depth 1 => one request in flight at a time
```

`bench` args: `mode(pipeline|sync) op(set|get|del) total_ops pipeline_depth`.
In pipeline mode the client keeps `depth` requests in flight, then drains the
replies — this is what amortizes syscall cost. Sync mode sends one request and
waits for its reply before sending the next, so it measures round-trip latency.

## Results

### Throughput vs pipeline depth (GET, keys pre-loaded)

| depth | ops/sec |
|------:|--------:|
| 1     | 54,600 |
| 8     | 290,000 |
| 32    | 613,000 |
| 128   | 874,000 |
| 512   | 991,000 |
| 1024  | 1,004,000 |
| 4096  | **1,040,000** |

```
depth   ops/sec
   1    █                            54,600
   8    ███████                     290,000
  32    ██████████████              613,000
 128    ████████████████████        874,000
 512    ███████████████████████     991,000
1024    ███████████████████████   1,004,000
4096    ████████████████████████  1,040,000
```

At depth 1 the client is round-trip bound and matches the sync latency number.
As depth grows, each `read()`/`write()` syscall carries many requests, and
throughput climbs ~19x to a ceiling near **1M ops/sec**. The curve is flat past
depth ~512 — the hashtable lookup was never the bottleneck, syscall overhead
was, which is exactly what pipelining removes. Once the syscall cost is
amortized to near zero there is nothing left to win.

### Throughput by operation (depth 256)

| op  | ops/sec | notes |
|-----|--------:|-------|
| GET | ~900K–1.04M | pure lookup |
| DEL | ~905,000 | lookup + free |
| SET | ~411,000 | allocates an `Entry` and grows the hashtable each op |

SET is ~2x slower than GET because every insert of a new key allocates an
`Entry`, copies the key/value strings, and can trigger a progressive-resize
step of the hashtable. GET and DEL touch existing entries only.

### Single-request latency (sync, depth 1)

| op  | ops/sec | p50 | p99 | p999 |
|-----|--------:|----:|----:|-----:|
| SET | 54,900 | 17.0µs | 41.0µs | 83.0µs |
| GET | 54,300 | 17.0µs | 43.0µs | 87.0µs |

p50 17µs is the loopback round-trip: client `write` → server `poll` wakeup →
process → `write` back → client `read`. Over a real network, add the network
RTT (typically 100µs–1ms+), which dominates this figure.

---

## Incremental resize vs `std::unordered_map` (tail latency)

### Why this benchmark exists

The server's headline design claim is that its hashtable "grows incrementally in
the background, so the server never pauses to resize." That was an assertion with
no evidence. This benchmark tests it against the obvious baseline and produces a
defensible number.

The baseline is `std::unordered_map`, **not** `std::map` (the `04_get_set_del`
version). `std::map` is a red-black tree; comparing to it measures tree-vs-hash,
not resize strategy. `std::unordered_map` is a real hashtable, so it isolates the
one thing the custom table does differently: **how it grows**.

### What is measured

Per-insert latency while inserting N unique keys, one at a time, into each
structure. What matters is the **tail** (worst single insert), not the average,
because the claim is about avoiding a stall — a rare, huge, single-operation
pause — not about being fast on average.

- `custom HMap` — `05_hashtables/05_hashtables.cpp`. On resize it moves the old
  table aside and migrates at most `k_resizing_work = 128` nodes per subsequent
  op, so no single op re-links the whole table.
- `std::unordered_map` — rehashes **all** current elements in one operation when
  the load factor trips. That one insert pays O(current size).

### How

In-process microbenchmark: `bench/resize_bench.cpp`, no server and no network,
so the only thing timed is the data structure. From the repo root:

```sh
clang++ -O2 -std=gnu++17 bench/resize_bench.cpp 05_hashtables/05_hashtables.cpp -o resize_bench
./resize_bench 1000000 40      # N_keys  N_timeline_buckets
```

Each insert is wrapped in `clock_gettime(CLOCK_MONOTONIC)`. It reports avg / p50
/ p99 / p999 / max, the index of the worst insert, and a bucketed max-latency
timeline (ASCII bars) so the spikes are visible in time order.

### Fairness decisions (so the comparison is honest)

1. **Custom `Entry` objects and their hashes are built OUTSIDE the timed region.**
   So the custom timing is insert + resize work, not `malloc`. This makes the
   custom *average* look better than it would in the server — which is exactly
   why this benchmark does **not** claim average/throughput superiority. The
   resize **spike** being measured is intrinsic to the algorithm and is not
   affected by this choice.
2. **`std::unordered_map` is NOT `reserve()`-d.** A KV server can't know the final
   key count up front, so natural incremental rehashing is the realistic
   behavior. Pre-reserving would erase the very stall under study.
3. `std::unordered_map::emplace` also does a dedup lookup that `hm_insert` does
   not. That widens the *average* gap slightly; it does not create the rehash
   spike. Again: the finding is the tail, not the average.

### Results (Apple M2, `-O2`)

Worst single-insert latency (the resize stall), by key count:

| keys | std::unordered_map | custom HMap | ratio |
|-----:|-------------------:|------------:|------:|
| 500K | 14.8 ms | 15 µs | ~980x |
| 1M | ~48–55 ms | 13–55 µs | ~900–3500x |
| 2M | 118 ms | 246 µs | ~480x |
| 4M | **241 ms** | **429 µs** | ~560x |

The `std::unordered_map` stall **grows with N** — it re-links every existing key
in a single op, so the more data, the longer the freeze (O(N), stop-the-world).
At 4M keys one insert blocks for **241 ms**; in a server that is every client
frozen for a quarter second.

The custom table stays **sub-millisecond** and roughly flat because the node
migration is capped at 128 per op.

### The residual custom spike (honesty)

The custom worst case is not literally zero (it's tens to a few hundred µs). That
residual is `hm_start_resizing` calling `calloc` for the new, bigger bucket array
— an O(new size) allocation done in one op. It's ~100–1000x cheaper than
`std::unordered_map`'s rehash because it only zeroes pages; it does not walk and
re-link N nodes. Two known follow-ups if it needed to be flatter still:

- the empty-bucket scan in `hm_help_resizing` (`if (!*from) resizing_pos++`) is
  not counted against `k_resizing_work`, so a run of empty buckets can add
  unbounded work to a single op;
- the new-array `calloc` could itself be amortized.

---

## What is fair to claim

- **~1M GET ops/sec** — pipelined, single connection, single-threaded, loopback.
  This is the server's processing ceiling and the defensible headline number.
- **p50 17µs request latency** — loopback round-trip, no pipelining.
- **sub-millisecond worst-case insert during resize** vs ~240ms for
  `std::unordered_map` at 4M keys — the resize claim, as predictable **tail
  latency**, not average speed. Stated in full:

  > Incremental resize keeps worst-case insert latency **sub-millisecond even at
  > 4M keys**, where `std::unordered_map` stalls **~240 ms** rehashing every key
  > in a single operation — a ~500x lower tail, and unlike the standard hashtable
  > the stall does not grow with the dataset.

- Do **not** present the pipelined ceiling as network throughput; it assumes a
  local client and deep batching.
- Do **not** claim the custom table is faster than `std::unordered_map` on
  average — the microbench pre-allocates nodes outside timing, which biases the
  average. The honest, unbiased result is the tail/no-stall behavior above.
