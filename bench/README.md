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

## Concurrency: one thread, many connections

### Why this benchmark exists

Chapter 03 is where the shape of this whole project gets decided, and the
argument I made there I made without measuring anything: of ten thousand
connected clients, only fifty have data ready at any instant, so a thread per
client pays to switch between ten thousand things in order to do work for fifty,
while the event loop — in the words I actually wrote — "skips them entirely."
Every benchmark above this section uses a single connection, which measures how
fast the server processes a request and says nothing at all about that argument.

The claim has three parts, and it took a benchmark to notice they aren't equally
true:

1. thousands of connections stay open on one thread,
2. an idle or stuck client doesn't hold up the others,
3. it avoids the memory cost of a thread per client.

Parts 1 and 3 hold, and the numbers are better than I expected. Part 2 holds in
the sense I actually cared about when I wrote chapter 03 and fails in the sense
the sentence literally says — and "skips them entirely" is simply wrong. The
event loop does not skip an idle connection. It walks past every single one of
them on every iteration, and this benchmark puts a price on each step.

### What is measured

Three separate questions, three modes in `bench/conn_bench.cpp`:

- **`idle N`** — open N connections that complete the TCP handshake and then say
  nothing, then run *one* active client in strict request/response mode and time
  its requests. Because only one client is doing work, whatever its latency does
  as N grows is the cost the N silent connections impose on it. That is the
  number chapter 03 assumed was zero.
- **`slow N`** — open N connections that pipeline ~4,000 requests each and then
  never read a byte of the replies, so the server's 4 KB `wbuf` fills, `write()`
  returns `EAGAIN`, and each of them wedges in `STATE_RES` permanently. Then run
  the same single active client. Comparing this against `idle N` at the same N
  isolates the cost of a client *misbehaving* from the cost of it merely
  *existing*.
- **`fanout N`** — drive all N connections at once with one request in flight on
  each, from a client-side `poll()` loop, and report aggregate throughput. This
  answers whether the server survives N genuinely active clients at all.

The `idle`-versus-`slow` pair is the one that matters most, because it is the only
way to separate the two things chapter 03 ran together.

### How

```sh
clang++ -O2 -std=gnu++17 05_hashtables/05_server.cpp 05_hashtables/05_hashtables.cpp -o server
clang++ -O2 -std=gnu++17 bench/conn_bench.cpp -o conn_bench
./server &
./conn_bench idle   5000 20000    # N silent conns   + 1 active client
./conn_bench slow   500  20000    # N wedged conns   + 1 active client
./conn_bench fanout 5000 100000   # N active conns, 1 request in flight each
```

Server memory comes from `ps -o rss=` on the server pid while the background
connections are held open, sampled after each run.

### Single-threaded, confirmed

`ps -M` on the running server reports one thread, and `pthread`, `std::thread`,
and `fork` appear nowhere in the source. This part was never in doubt, but it is
worth stating that everything below happens on that one thread.

### Result: connections are cheap to hold and expensive to have

One active client, N silent background connections:

| idle conns | active p50 | active p99 | active ops/sec | server RSS | RSS per conn |
|-----------:|-----------:|-----------:|---------------:|-----------:|-------------:|
| 0     | 22 µs | 61 µs | 40,956 | 1.3 MB | — |
| 500   | 160 µs | 343 µs | 5,891 | 6.8 MB | 10.9 KB |
| 1,000 | 321 µs | 874 µs | 2,875 | 12.1 MB | 10.8 KB |
| 2,000 | 661 µs | 2,372 µs | 1,323 | 22.9 MB | 10.8 KB |
| 5,000 | 1,961 µs | 4,781 µs | 464 | 50.8 MB | 9.9 KB |

The memory column is the happy half. Roughly 10 KB of resident memory per
connection, flat across the whole range, which is just the `Conn` struct — two
4 KB buffers plus a little bookkeeping — allocated once per client and never
grown. Five thousand connections cost fifty megabytes. The thread-per-client
design chapter 03 rejected would reserve a 512 KB stack per client on macOS
(8 MB on Linux) plus a scheduler entry apiece, which is 2.5 GB of stack for the
same five thousand clients before a single byte of application state exists. The
memory argument in chapter 03 was right, and this is the measurement behind it.

The latency column is the half I got wrong. An active client's p50 goes from
22 µs to 1,961 µs — eighty-nine times worse — because of connections that are
doing nothing whatsoever. Subtracting the baseline shows the cost is not just
present but linear:

```
idle conns   added p50 over baseline   cost per idle conn
    500              138 µs                 0.28 µs
  1,000              299 µs                 0.30 µs
  2,000              639 µs                 0.32 µs
  5,000            1,939 µs                 0.39 µs
```

About 0.3 µs of added latency per idle connection, per request. For a `GET` that
does roughly 1 µs of real work, three idle connections already cost more than the
lookup they are delaying.

### Where that 0.3 µs comes from

The interface is the explanation. `poll()` takes a pointer to an array of
`pollfd` and a count, and every part of servicing that call scales with the
count, not with how many descriptors are actually ready:

1. the kernel copies the entire array in from user space — N entries, whatever
   the answer turns out to be;
2. it walks every entry in turn and asks that descriptor's underlying file
   object whether the events I asked for are ready right now;
3. it copies the array back out so I can read `revents`.

Nothing in that sequence can be skipped for a socket with no data, because the
only way to discover a socket has no data is to go and look. Servicing one
readable connection out of five thousand costs the same full walk as servicing
all five thousand, which is exactly the "pay for ten thousand to serve fifty"
cost I thought I had escaped by leaving threads behind. I moved it from the
scheduler into the syscall.

My loop then adds a second, independent linear cost on top, before the syscall
even happens:

```cpp
poll_args.clear();
for (Conn *conn : fd2conn) {
    if (!conn) continue;
    struct pollfd pfd = {};
    pfd.fd = conn->fd;
    pfd.events = (conn->state == STATE_REQ) ? POLLIN : POLLOUT;
    pfd.events = pfd.events | POLLERR;
    poll_args.push_back(pfd);
}
```

Every iteration throws the array away and rebuilds it from scratch, so N pushes
and N branches in user space precede the kernel's N. The set of connections
barely changes between iterations; almost all of that work reconstructs something
identical to what was just discarded. Two linear costs stacked, both paid in
full to service one ready socket.

The 0.3 µs measured above is their sum. This benchmark does not separate them —
see the honesty section below.

### Result: a wedged client costs no more than a silent one

Same single active client, but now the N background connections are the
pathological case: each has pipelined thousands of requests and will never read a
reply, so the server has filled their write buffers, hit `EAGAIN`, and parked
them in `STATE_RES` with no way forward.

| stuck conns | active p50 | active p99 | active ops/sec |
|------------:|-----------:|-----------:|---------------:|
| 1   | 28 µs | 71 µs | 31,648 |
| 10  | 29 µs | 61 µs | 32,391 |
| 100 | 42 µs | 149 µs | 20,529 |
| 500 | 167 µs | 606 µs | 5,186 |

Five hundred permanently wedged clients cost an unrelated active client 167 µs at
p50; five hundred silent ones cost it 160 µs. The difference is inside the run-to-
run noise, which means a client that has stopped reading is worth exactly as much
trouble as a client that has said nothing at all — and the entire cost of a
background connection is the poll scan, not what the client is doing.

This is the part of chapter 03's design that works, and it is the part worth
being precise about. Non-blocking sockets guarantee the thread never falls asleep
inside `read()` or `write()`, and the per-connection state machine means a
connection that cannot make progress simply stays in `STATE_RES` and gets skipped
on the next pass rather than holding the loop while it waits. No client can
*block* another client. That is a strictly weaker statement than the one I wrote
in chapter 03, and it is the one the code actually earns.

### Result: five thousand active connections

All N connections driven at once, one request in flight on each:

| conns | aggregate ops/sec | p50 | connect rate |
|------:|------------------:|----:|-------------:|
| 100   | 139,585 | 553 µs | 10,499 conn/sec |
| 1,000 | 110,577 | 6.7 ms | 4,837 conn/sec |
| 5,000 | 110,819 | 32.6 ms | 1,025 conn/sec |

The server holds five thousand simultaneous active connections and sustains about
110K ops/sec across them without dropping a connection or crashing, and aggregate
throughput is flat from one thousand to five thousand, so the loop is not
collapsing under the count — it is doing the same total amount of useful work and
dividing it among more waiters, each of whom pays the O(N) scan. That is why
per-request latency climbs while throughput does not fall.

Connection establishment degrades tenfold over the same range, and that has a
separate cause worth naming: `main()` calls `accept_new_conn()` once per `poll()`
iteration, so the server accepts at most one new client per pass through a loop
that is itself O(N). With `kern.ipc.somaxconn` at 128 on macOS, a burst of
connects arriving faster than that overflows the backlog and the kernel starts
refusing them.

### Fairness decisions and what these numbers don't show

1. **The two linear costs are measured together, not apart.** The 0.3 µs figure
   is the sum of the user-space rebuild and the kernel's scan, and this harness
   cannot attribute it between them. Splitting it would need either a profiler on
   the server or a variant that maintains `poll_args` incrementally; until then,
   "`poll()` is O(N)" is the correct conclusion and "the rebuild costs X of it"
   is not established.
2. **The `fanout` p50 column is an upper bound, not a latency measurement.** The
   client is itself single-threaded and issues all N requests before it polls, so
   a request sent early in a batch waits for the rest of the batch to be written
   before anyone reads a reply. Aggregate ops/sec from `fanout` is meaningful;
   its p50 is not comparable to the `idle`/`slow` tables, which time one request
   at a time on an otherwise quiet client.
3. **Everything is loopback on one machine.** The client competes with the server
   for the same eight cores, which flatters nothing and hurts both, and there is
   no network RTT to hide the per-request cost behind. On a real network the
   0.3 µs per idle connection would be a smaller fraction of end-to-end latency
   without being any smaller in absolute terms.
4. **The connection ceiling here is the ephemeral port range, not the server.**
   macOS hands out ports 49152–65535, so a single client host can hold roughly
   16K connections to one port regardless of what the server could manage.
   Five thousand is comfortably inside that; it is not the server's limit, and
   this benchmark does not find the server's limit.

### What this creates

Chapter 03 traded the scheduler's O(N) for `poll()`'s O(N) and I did not notice,
because with one client on the other end the two are indistinguishable. The fix
is not a different loop shape — the state machine and the non-blocking sockets
are doing their job — it is a different question to ask the kernel. `kqueue` on
macOS and `epoll` on Linux let you register interest in a descriptor *once* and
then ask only for the descriptors that became ready, which deletes both linear
costs at the same time: nothing to rebuild in user space, and nothing for the
kernel to walk. The idle-connection tax should go to approximately zero, and the
right way to know is to port the loop and rerun `idle 5000` against it.

Two smaller things the benchmark surfaced along the way: `accept_new_conn()`
should loop until `EAGAIN` rather than taking one client per pass, and a
connection that opens and never speaks is currently held forever, because
nothing in the server times a connection out.

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

- **5,000 concurrent connections on one thread at ~10 KB of RSS each** — 50 MB
  total, against a 512 KB stack per client for the thread-per-connection design
  this was built to avoid, while sustaining ~110K ops/sec aggregate across all
  5,000.
- **No head-of-line blocking**, as a measured statement rather than an assumed
  one. Stated in full:

  > 500 clients wedged mid-response — pipelined thousands of requests, never
  > reading a reply, parked in `STATE_RES` with a full write buffer — cost an
  > unrelated active client **167 µs p50, against 160 µs for 500 silent
  > connections**. A client that stops making progress is worth no more trouble
  > than one that never spoke, because non-blocking sockets and the
  > per-connection state machine mean the loop never waits on any single client.

- Do **not** present the pipelined ceiling as network throughput; it assumes a
  local client and deep batching.
- Do **not** claim the custom table is faster than `std::unordered_map` on
  average — the microbench pre-allocates nodes outside timing, which biases the
  average. The honest, unbiased result is the tail/no-stall behavior above.
- Do **not** say idle clients are free, or repeat chapter 03's "skips them
  entirely." `poll()` walks every registered descriptor on every iteration, which
  costs ~0.3 µs per idle connection per request and takes an active client's p50
  from 22 µs to 1,961 µs at 5,000 idle connections. The defensible line is that a
  slow client cannot **block** another client — not that it cannot **slow it
  down**.
