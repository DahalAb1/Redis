# Building Redis From Scratch

This is a ground-up implementation of a Redis-style key-value server in C++ — built without frameworks or abstractions in the way, working directly with sockets, the kernel, and bytes on the wire.

Each chapter introduces *one* problem and the smallest correct fix for it. Read in order, the working server emerges from `socket(2)` in five steps.

---

## The journey

### [01_client_server](./01_client_server/) — the call that lied

I wanted to send a string from one program to another, so I learned the five-call ceremony — `socket`, `setsockopt`, `bind`, `listen`, `accept` — and wrote a server that did one thing: `read(connfd, rbuf, 6)`. The client wrote `"hello"`. The server printed `"hello"`. It looked finished.

What I didn't see was that I'd asked for 6 bytes and happened to get them, and that this was luck. If the message had been longer, or the client farther away than localhost, the same `read()` would have returned partway through — `"he"`, maybe, or `"hell"` — and the rest would have shown up on the next call. My code had no idea that was even a possibility, because in my head a `read()` corresponded to a `write()`. One message in, one message out.

The model that breaks it: TCP doesn't carry messages at all. It carries a stream of bytes. The sending kernel might split your `write("hello")` across packets, or batch it with the next one. The receiving kernel just dumps whatever arrives into a [flat buffer](#kernel-send-and-receive-buffers), and `read()` scoops out whatever's there. If the sender wrote `"hello"` then `"world"`, the receiver's first `read()` might return `"hellow"`. There's no syscall anywhere that hands you exactly one message back — at this layer there is no such thing as a message.

So my "working" code was a bug waiting to be tested by a longer string or a slower link. The fix couldn't come from a smarter `read()`; it had to come from a layer above the kernel. I had to invent the concept of a message myself.

### [02_protocol_client_server](./02_protocol_client_server/) — inventing messages

If the kernel won't tell you where a message ends, the message has to tell you itself. The simplest thing that works: prefix every message with its length. Four bytes of `uint32_t`, then that many bytes of payload. The receiver reads exactly 4 bytes, decodes a number, then reads exactly that many more.

That word *exactly* is doing a lot of work. A single `read()` might return fewer bytes than I asked for, for the same reason as in chapter 01. So I wrapped it in a loop:

```cpp
read_full(fd, buf, n);   // keep calling read() until n bytes have arrived
write_all(fd, buf, n);   // same idea for write()
```

This is the moment the mental model from chapter 01 actually finishes inverting. A "message" is not a thing TCP gives you. It's a contract you and the other side agree to honour, sitting one layer above the byte hose. You define the framing, you parse the framing, you enforce the framing. The transport doesn't care.

With framing in place, the server could finally handle more than one request on a connection — `one_request()` in a loop until the client disconnected. But it still served one *client* at a time. As soon as the server entered `read_full()` waiting on client A, the kernel parked the entire thread on a wait queue. Client B could connect and sit there for hours; the server wouldn't even know it existed until A spoke. That isn't a bug in `read_full` — it's a property of the only tool I had. To serve many clients on one thread I'd need a fundamentally different way of asking the kernel "is anybody ready yet?"

### [03_event_polling](./03_event_polling/) — many clients, one thread

The obvious fix to "one client at a time" is a thread per client. It also doesn't work, and the reasons it doesn't work are worth understanding because they're the reason Redis is single-threaded in the first place.

Each thread reserves a stack — 8 MB by default on Linux. Ten thousand connected clients means 80 GB of address space gone, just for stacks. Worse, every time the OS switches the CPU from one thread to another it costs roughly 1-5 microseconds. A Redis `GET` is about 1 microsecond of real work. So most of the CPU's time would be spent shuffling threads in and out, not answering queries. And since all those threads share one hashtable, you'd need mutexes around every operation, and the contention on those mutexes would become its own bottleneck.

Then I noticed the thing that makes this whole approach unnecessary: of those ten thousand connected clients, at any given instant maybe fifty actually have data ready to be read. The other 9,950 are idle, waiting on the user, the network, whatever. Dedicating a thread to each of them is paying the cost of switching between 10,000 things to do work for only 50.

The alternative: ask the kernel directly. `poll()` takes an array of file descriptors and returns the ones that are actually ready. Hand it the listener and every connection; it tells you which ones have something to do. One thread, no switching, no mutexes.

There's one catch. `poll()` says "fd 7 is readable," but it can be slightly wrong, or only 12 bytes might have arrived when I want 4096. In blocking mode, `read()` would then sleep waiting for the rest — and the whole event loop would sleep with it. So every fd has to be set non-blocking with `fcntl(fd, F_SETFL, O_NONBLOCK)`. After that, `read()` either returns whatever's available immediately, or returns -1 with `errno = EAGAIN` ("nothing right now, try again later"). The thread never sleeps inside a syscall. `poll()` and non-blocking I/O are complementary — `poll()` tells you which fds are worth touching, and non-blocking guarantees that touching them is safe.

This forces the code to change shape. With blocking I/O, a connection lived inside one function call — you read, you process, you write, you return. With non-blocking I/O, that function might be entered four times before a single message has fully arrived. So a connection becomes a small state machine — `STATE_REQ` while it's reading, `STATE_RES` while it's writing, `STATE_END` when it's done — with its own read and write buffers that persist between calls. The main loop pokes each ready connection once per tick and lets it advance one step.

A nice thing falls out for free. If three requests arrived in a single TCP packet, the read buffer now contains all three. After parsing the first, I `memmove` the leftover bytes to the front of the buffer and try again. The server pipelines without ever explicitly being told to.

### [04_get_set_del](./04_get_set_del/) — teaching it what to do

By now the server scales beautifully and does nothing of value. It echoes. To make it a key-value store I needed `GET key`, `SET key val`, `DEL key` — which means parsing structure out of the framed payload.

The trick is that protocols nest. Chapter 02's outer frame says "here are N bytes of payload." Those N bytes are now themselves a small protocol: a 4-byte count of how many strings, followed by each string as length + bytes. Effectively argv on the wire. The choice to length-prefix every string instead of delimiting them with a special byte (like CRLF) is deliberate — lengths are O(1) to validate against the remaining buffer, while delimiter scanning is O(n) and breaks the moment a value contains the delimiter byte. The framing layer never has to *look inside* the bytes; it just measures them.

`parse_req()` walks the buffer and produces a `vector<string>`. `do_request()` switches on `cmd[0]` (case-insensitive via `strcasecmp` — small detail, but it's the difference between a usable interface and a fragile one) and routes to one of three handlers. The response has a mirror structure: a 4-byte result code (`RES_OK`, `RES_ERR`, `RES_NX`) followed by an optional value, all wrapped in the same outer length-prefix frame. The client does the same framing dance in reverse.

The handlers themselves are deliberately boring — they poke a `std::map<string, string>`. A `std::map` is a red-black tree, which means every operation is O(log n) *and* every node is its own separate heap allocation, scattered across wherever the allocator happened to put it. Walking the tree means jumping around in memory, and every jump risks a **cache miss** — roughly a 100× slowdown when the CPU has to leave its on-chip cache and go fetch from RAM. This is the **data-locality** lens, and for a system whose hot path is "look up a small value by key, return it now," it's the lens that matters most: the algorithmic complexity is almost a sideshow next to the memory layout. `std::map` is structurally wrong on both counts. Chapter 05 picks up exactly this thread.

### [05_hashtables](./05_hashtables/) — earning the swap

Now the placeholder gets replaced, and the design is shaped by two pressures. The obvious one: every operation should be fast, because every client request hits this code path. The less obvious one: the hashtable has to *grow* without ever blocking the event loop, because the entire single-threaded design from chapter 03 falls apart the moment one operation takes 50 milliseconds.

Three ideas, each chosen against a specific cost.

**Power-of-2 bucket counts.** Every operation has to map a hash to a bucket: `bucket = hash % n`. The `%` operator is integer division, which is one of the slower things a CPU can do on the hot path. But if `n` is a power of 2, `hash % n` equals `hash & (n - 1)` — bitwise AND, one cycle. Why? In binary, a power of 2 looks like `1` followed by zeros: `8 = 1000`. Subtract one and you get a clean run of low-order ones: `7 = 0111`. ANDing with that mask keeps the low bits and wipes the rest — which is exactly the remainder. So `h_init` asserts the bucket count is a power of 2 and precomputes `mask = n - 1`. The assertion isn't paranoia; it's the precondition that makes the bitwise trick correct everywhere downstream.

**Intrusive nodes.** This is the most elegant of the three, and it's the natural extension of the data-locality lens from chapter 04. The conventional design has the data structure *own* its data: each hashtable node holds a pointer to a value that lives somewhere else in memory. Walking a chain means load the node, read the value pointer, then go fetch the value from RAM — and that last fetch is the same ~100× cache miss that made `std::map` so bad. You've changed the algorithm from O(log n) to O(1), but the actual bottleneck — chasing pointers across scattered allocations — is identical.

The intrusive design inverts the ownership. The data structure doesn't hold the data; the data *hosts* the structure. The `HNode` is a field inside the `Entry` struct, sitting in the same allocation as the key and value:

```cpp
struct Entry {
    HNode       node;   // the hashtable's bookkeeping...
    std::string key;    // ...lives next to the data
    std::string val;    // it's bookkeeping for
};
```

When the CPU pulls the node into cache to compare hashes during a chain walk, the key and value come along in the same cache line. One trip per hop, not two. This is the design exploiting **spatial locality** — the fact that bytes sitting near each other in memory tend to get pulled into cache together. Worth being precise here: this isn't "cache-aware" in the formal sense (a true cache-aware structure tunes itself to specific cache line sizes and the L1/L2/L3 hierarchy — think B-trees with nodes sized to fit one cache line). It's just **cache-friendly** — a memory layout that happens to respect how CPUs already want to read memory. The shape of the data matches the shape of the access pattern.

The trade-off is that you've lost the obvious way to get back from a node pointer to the entry that contains it — the `Entry*` is no longer stored anywhere, because the node *is* part of the entry. The fix is `container_of`: given a pointer to a struct field, subtract the field's offset within the struct to recover the pointer to the struct itself. It's the inverse of `offsetof`, and it sounds like a dirty pointer trick until you realize it's exactly the operation that makes the embedding coherent. The same pattern runs throughout the Linux kernel's intrusive linked lists; once you've seen it a few times it stops feeling like a trick and starts feeling like the obvious move whenever a data structure needs to know what contains it.

**Progressive resizing.** This is the subtle one. When the table fills up (load factor 8 in this code), you have to grow it. The naive thing is to allocate a table twice the size and rehash every existing entry into it. For a table with a million entries that's tens of milliseconds — and for those tens of milliseconds, the event loop is frozen. Every connected client is waiting. The whole single-threaded design from chapter 03 was built to avoid exactly this kind of stall.

So instead: allocate the new table, but don't rehash anything yet. Keep the old table alive. Every subsequent operation does a tiny amount of moving work — up to 128 nodes from the old table to the new one. Lookups check the new table first, then fall back to the old. Inserts always go to the new. Deletes try both. After a few thousand operations the old table is empty and gets freed. The user never sees a stall, because the cost of growth has been smeared invisibly across thousands of normal operations.

That's the shape of the project so far: each chapter solves one problem that the previous chapter created, and the solution always involves understanding what the layer below actually guarantees — instead of what I'd assumed it guaranteed.

---

## Final architecture (after chapter 05)

```
            ┌──────────────────────────────────────────────────┐
            │                    Event loop                    │
            │   poll()  ──►  one thread, no context switches   │
            └────────────────────┬─────────────────────────────┘
                                 │ readable / writable fds
                                 ▼
            ┌──────────────────────────────────────────────────┐
            │           Per-connection state machine           │
            │       STATE_REQ ──► STATE_RES ──► STATE_END      │
            │       rbuf (read)            wbuf (write)        │
            └────────────────────┬─────────────────────────────┘
                                 │ parsed request (vector<string>)
                                 ▼
            ┌──────────────────────────────────────────────────┐
            │          Command dispatch (GET/SET/DEL)          │
            └────────────────────┬─────────────────────────────┘
                                 │
                                 ▼
            ┌──────────────────────────────────────────────────┐
            │     Intrusive hashtable (HMap → ht1 + ht2)       │
            │     progressive resizing, FNV-1a, mask = n-1     │
            └──────────────────────────────────────────────────┘
```

---

## Numbers

Every design decision above is a claim, and claims are cheap. So I measured them.

| | |
|---|---|
| **~1.04M GET ops/sec** | pipelined, single connection, single thread, loopback |
| **p50 17µs** | request latency, no pipelining — a loopback round-trip |
| **429µs worst insert @ 4M keys** | vs **241ms** for `std::unordered_map` — the progressive-resize claim, measured |

That last row is the one I care about. "The user never sees a stall" was an assertion until there was a number next to it, and the honest version of that number is a *tail latency* result — not "faster than the standard library," which the benchmark does not show and does not claim.

**[Full method, fairness caveats, and what these numbers don't prove → `bench/`](./bench/)**

---

## Chapter 01 — Talking over TCP

**Problem.** Two programs need to exchange bytes over a network.

**Solution.** Raw BSD sockets. The server walks five steps: `socket → setsockopt(SO_REUSEADDR) → bind → listen → accept`. Each `accept()` returns a fresh fd dedicated to that client; the original fd keeps listening. The client is shorter: `socket → connect → write → read`.

```cpp
int fd = socket(AF_INET, SOCK_STREAM, 0);             // IPv4 + TCP
setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sz);   // restart-friendly
bind(fd, ...);                                        // claim port 1234
listen(fd, SOMAXCONN);                                // accept queue
int connfd = accept(fd, ...);                         // blocks per client
```

**Quietly broken.** The server's `read(connfd, rbuf, 6)` assumes the kernel hands back the whole message in one call. TCP doesn't promise that — it's a byte stream. With short messages on localhost, this works 99% of the time, which is the worst kind of bug.

→ See [TCP is a byte stream, not a message stream](#tcp-is-a-byte-stream-not-a-message-stream).

---

## Chapter 02 — Framing messages

**Problem.** TCP doesn't preserve message boundaries. The receiver can't tell where one message ends and the next begins.

**Solution.** A length-prefix protocol. Every message on the wire is:

```
┌─────────┬──────────────────────────────┐
│ 4 bytes │   payload  (≤ k_max_msg)     │
│  len    │                              │
└─────────┴──────────────────────────────┘
```

Read exactly 4 bytes (the header), decode the length, then read exactly that many bytes (the payload). To do that, you need helpers that *loop* until enough bytes have arrived:

```cpp
read_full(fd, buf, n);   // loops read() until n bytes are read
write_all(fd, buf, n);   // loops write() until n bytes are written
```

With framing in place, the server finally handles multiple requests on one connection — `one_request()` runs in a loop until the client closes.

**Still broken.** The server is single-client. While it's blocked in `read_full()` waiting on client A, every other client is frozen. Threads-per-client doesn't fix it either.

→ See [Why event polling, not threads or processes](#why-event-polling-not-threads-or-processes).

---

## Chapter 03 — One thread, many clients

**Problem.** A blocking server serves one client at a time. Threads/processes don't scale to thousands of mostly-idle connections.

**Solution.** An event loop. One thread parks in `poll()` until the kernel reports which fds are ready, then services only those. Every fd is set non-blocking — syscalls return `EAGAIN` instead of sleeping when there's no data. Each connection gets a state machine and its own buffers:

```cpp
struct Conn {
    int      fd;
    uint32_t state;                  // STATE_REQ | STATE_RES | STATE_END
    uint8_t  rbuf[4 + k_max_msg];   size_t rbuf_size;
    uint8_t  wbuf[4 + k_max_msg];   size_t wbuf_size, wbuf_sent;
};
```

The main loop, every tick:

1. Build a `pollfd[]`: listener wants `POLLIN`; each connection wants `POLLIN` if reading or `POLLOUT` if writing.
2. `poll()` blocks until something is ready (1 s timeout).
3. For each fd with non-zero `revents`, call `connection_io()` → `state_req()` or `state_res()`.
4. Reap any connection that hit `STATE_END`.

`try_one_request()` gets pipelining for free: after parsing a complete message out of `rbuf`, it `memmove`s the leftover bytes to the front and tries again. If three requests arrived in one TCP packet, all three are processed before going back to `poll()`.

→ See [Blocking vs non-blocking I/O](#blocking-vs-non-blocking-io), [EINTR](#eintr--retrying-interrupted-syscalls), [pollfd](#pollfd).

**Still limited.** The "command" is just an echo. There's no concept of keys or operations.

---

## Chapter 04 — GET, SET, DEL

**Problem.** The server has plumbing but no semantics.

**Solution.** A second-level protocol *inside* the framed payload. The payload is a list of length-prefixed strings — argv on the wire:

```
payload = [4B nstr] [4B len][bytes] [4B len][bytes] ...
```

`parse_req()` walks the buffer and produces a `vector<string>`. `do_request()` switches on `cmd[0]` (case-insensitive via `strcasecmp`) and routes to `do_get` / `do_set` / `do_del`. The response carries a code:

```
response = [4B reslen] [4B rescode] [optional value bytes]
rescode  ∈ { RES_OK = 0,  RES_ERR = 1,  RES_NX = 2 }     // OK / err / not-found
```

Storage is a placeholder `std::map<std::string, std::string>`. It works; it's just slow and not what Redis is.

The client takes argv directly:

```bash
./client.out set foo bar
./client.out get foo
./client.out del foo
```

**Still limited.** `std::map` is a red-black tree — O(log n) per op, plus cache-unfriendly node allocations everywhere. Redis is a hashtable.

---

## Chapter 05 — A real hashtable

**Problem.** Need O(1) average-case `get`/`set`/`del`, *and* a way to grow the table without ever blocking the event loop with a giant rehash.

**Solution.** An intrusive hashtable with chaining and progressive resizing.

```cpp
struct HNode { HNode *next;  uint64_t hcode; };           // node
struct HTab  { HNode **tab;  size_t mask;  size_t size; };// one table
struct HMap  { HTab ht1;     HTab ht2;     size_t resizing_pos; }; // two tables
```

Three ideas at work:

1. **Intrusive nodes.** `HNode` lives *inside* the payload struct (`Entry { HNode node; string key; string val; }`). Walking a chain pulls the key/value into cache for free — no second pointer chase. The `container_of` macro recovers the `Entry*` from an `HNode*` by subtracting the field offset.

2. **Power-of-2 buckets.** `HTab` requires `n` to be a power of 2 and precomputes `mask = n - 1`. Every lookup is `hcode & mask` — no division. This is on the hot path of every op.

3. **Progressive resizing.** When `ht1`'s load factor hits 8, it gets moved to `ht2` and a new (2×) `ht1` is allocated. `ht2` stays alive as a read-through fallback. Every subsequent op moves up to 128 nodes from `ht2` to `ht1` (`hm_help_resizing`). When `ht2` empties, it's freed. Growth is amortized — no single op stalls.

Lookups check `ht1`, then `ht2`. Inserts go into `ht1`. Deletes try both. Hash function is **FNV-1a** (`str_hash`) — small, fast, decent distribution.

→ See [Why power of 2 buckets](#why-power-of-2-buckets), [Intrusive data structures](#intrusive-data-structures-and-cache-locality).

---

## Build & run

Each chapter is self-contained:

```bash
# build server (chapter 05 has the multi-file case)
cd 05_hashtables
g++ -Wall -O2 -std=c++17 -o server.out 05_server.cpp 05_hashtables.cpp
./server.out

# in another terminal — chapter 04's client speaks the same protocol
cd ../04_get_set_del
g++ -Wall -O2 -std=c++17 -o client.out 04_client.cpp
./client.out set foo bar
./client.out get foo
./client.out del foo
```

Earlier chapters build with a single `g++ -o out file.cpp`.

---

## Repo map

```
01_client_server/             baseline TCP echo
02_protocol_client_server/    + length-prefix framing
03_event_polling/             + non-blocking + poll() event loop
04_get_set_del/               + GET/SET/DEL over std::map
05_hashtables/                + custom intrusive hashtable
bench/                        throughput + resize-latency benchmarks
self_test/                    scratchpad — gitignored
```

---

# Notes from the trenches

The *why* behind decisions in the code, in my own voice. Read these once and the source files start to feel inevitable.

## Sockets & I/O

### TCP is a byte stream, not a message stream

When you call `write(fd, "hello", 5)`, you hand 5 bytes to the kernel. The kernel doesn't immediately send `"hello"` as-is — it puts those bytes in a send buffer and decides *when* and *how much* to send based on packet size, congestion, whatever. On the receiving side, packets arrive and the kernel dumps payloads into a flat receive buffer. `read()` scoops out of that buffer.

```
Client:    write("hello")  write("world")
                  │              │
Send buf:   [h][e][l][l][o][w][o][r][l][d]
                  │  (split into packets however)
Recv buf:   [h][e][l][l][o][w][o][r][l][d]
                  │
Server:    read() → "hellow"   read() → "orld"
```

There are *no walls* between writes. The "stream" isn't a consequence of physical wires — packets on the wire have boundaries. It's a deliberate design choice: TCP's job is reliable, ordered byte delivery, not message framing. UDP preserves boundaries (each `recv()` is one datagram); TCP doesn't. Framing is your application's job. That's the whole reason chapter 02 exists.

### Kernel send and receive buffers

Every TCP socket has two buffers in kernel memory dedicated to it: a **send buffer** on the sender's side and a **receive buffer** on the receiver's side. Each is typically a few hundred KB. They're the actual machinery TCP runs on — every other behaviour in this section follows from how they work.

When you call `write(fd, buf, n)`, the kernel **copies** your bytes into the send buffer and returns. It does *not* wait for those bytes to leave the machine. The kernel's TCP code then transmits whatever it can from the buffer whenever it can — based on the receiver's advertised window, the network's congestion state, MTU, timers — possibly splitting your one `write()` across many packets, or coalescing it with the next one. `write()` returning is decoupled from "the bytes being sent" entirely.

When packets arrive on the receiving machine, the kernel reassembles them in order and dumps the payload bytes into the receive buffer. Each `read(fd, buf, n)` just pulls up to `n` bytes out of whatever's currently sitting there. There's no record of which bytes came from which `write()` — the buffer is a flat queue, message boundaries don't exist in it.

Four behaviours fall out of this directly, and they explain almost everything about how the I/O code in this project is shaped:

- **Partial reads.** `read()` returns whatever the receive buffer happens to hold *right now* — could be a fragment, could be multiple messages worth, could be nothing yet. This is the underlying reason the framing protocol from chapter 02 exists.
- **Partial writes.** If the send buffer is nearly full, `write()` only copies as much as fits and returns the actual count. Hence `write_all()` looping until everything has been handed off.
- **`EAGAIN` on a non-blocking `write()`.** Means the send buffer is full — there's literally no room to copy more. The kernel can't accept the bytes until its TCP code drains some of them onto the wire. Try again after `poll()` reports `POLLOUT`.
- **Free pipelining.** Three small requests sent back-to-back can all sit in the receive buffer at once. A single `read()` pulls them all into the user-space `rbuf`, and `try_one_request()` parses them one at a time without going back to the kernel. The user-space `rbuf` and `wbuf` are essentially second-tier buffers mirroring the kernel ones — necessary because a single message can span multiple `read()` calls, so the application has to accumulate across them.

### Blocking vs non-blocking I/O

Every fd starts in **blocking** mode. On a blocking fd, `read(fd, buf, 4096)`:

- returns the data if some has arrived,
- **puts your thread to sleep** if nothing has — the thread is removed from the run queue and sits there until bytes arrive,
- returns 0 if the connection is closed.

Same for `write()`: if the socket's send buffer is full, the thread sleeps until space opens.

Set with `fcntl(fd, F_SETFL, flags | O_NONBLOCK)` and the same calls behave differently:

- returns the data if some has arrived,
- returns -1 with `errno = EAGAIN` if there's no data right now — *no sleeping*,
- returns 0 if closed.

`poll()` tells you "fd 7 is readable" — but that might mean only 12 bytes arrived when you wanted 4096. In blocking mode, the next `read()` would sleep waiting for the rest. In non-blocking mode, you take the 12 bytes and accumulate (`try_fill_buffer` does this with `rbuf`). The event loop and non-blocking I/O are *complementary* — you need both. `poll()` says which fds are ready; non-blocking guarantees the thread *never* sleeps inside `read`/`write`.

### Why event polling, not threads or processes

Redis uses a single thread. With 10,000 clients, how does that thread know which one has data ready?

**Approach 1 — `fork()` per connection.** 10,000 processes. Each has its own memory space, stack, kernel metadata. The CPU runs N at once (cores). The OS time-slices: run A, switch to B, switch to C. Process context switches cost ~5 μs because the TLB (the CPU's address-translation cache) gets flushed each time. A Redis `GET` is ~1 μs of real work. The switching dwarfs the work.

**Approach 2 — thread per connection.** No TLB flush, but each thread needs an 8 MB stack. 10,000 threads = 80 GB of reserved stack. Switches still cost ~1-5 μs. And all threads share the hashtable, so you need mutexes — contention becomes the bottleneck.

**Approach 3 — event loop.** One thread, one stack, zero context switches. `poll()` says "fds 5, 9, 23 have data." Serve those three (~3 μs total), park back in `poll()`. The key insight: at any instant, of 10,000 clients, maybe 50 have data ready. The other 9,950 are idle. Threads waste time switching between threads with nothing to do; the event loop skips them entirely.

### `pollfd`

```c
struct pollfd {
    int   fd;        // the fd to monitor
    short events;    // what YOU want to watch (POLLIN = readable, POLLOUT = writable)
    short revents;   // what ACTUALLY happened (kernel fills this in)
};
```

You fill `fd` and `events`. The kernel fills `revents`. After `poll()` returns, loop the array and only handle entries where `revents` is non-zero — those are the active fds.

## Unix patterns

### EINTR — retrying interrupted syscalls

```c
do {
    rv = read(conn->fd, buf, cap);
} while (rv < 0 && errno == EINTR);
```

Signals are async notifications the kernel delivers (`SIGCHLD` when a child exits, `SIGALRM` from a timer, `SIGINT` from Ctrl+C). If a signal arrives while your thread is inside a blocking syscall, the kernel pauses the syscall to run the handler. Afterwards, rather than resuming the syscall, the kernel returns -1 with `errno = EINTR` — "you got interrupted, nothing was read."

Nothing broke. No data was lost. The socket is fine. The `read()` just didn't get a chance. So you retry immediately. It's a one-comparison cost per call. *Not* retrying means a random signal at the wrong moment silently drops a perfectly healthy connection — a bug nearly impossible to reproduce.

### `errno` gets clobbered

`errno` is a global set by the *next* syscall. The moment you see a failure, snapshot it: `int err = errno;`. Otherwise the next `printf` or `close` overwrites it and your error message lies.

## Bytes & wire format

### Byte order (endianness)

The number `0x01020304` can be stored two ways:

|                | byte 0 | byte 1 | byte 2 | byte 3 |
|----------------|--------|--------|--------|--------|
| Big-endian     | `01`   | `02`   | `03`   | `04`   |
| Little-endian  | `04`   | `03`   | `02`   | `01`   |

The internet standardized on big-endian ("network byte order"). The conversion functions:

- `htons` / `htonl` — host→network short / long
- `ntohs` / `ntohl` — the reverse

They're smart: on a big-endian machine they're no-ops; on little-endian they swap. Always call them for ports and IPs. The protocol headers in this project cheat — `memcpy(&len, rbuf, 4)` assumes little-endian — which is fine on a single machine, not portable.

## Hashtable internals

### Why power of 2 buckets

Every op needs to map a hash to a bucket index:

```
hash("name") = 928374
928374 % 8 = 6   →  bucket 6
```

`%` is a division — not free. If `n` is a power of 2, you can replace it with bitwise AND:

```
928374 % 8  ==  928374 & 7
```

Mechanics: powers of 2 in binary have one 1 bit. Subtract 1 and every bit below flips to 1.

```
8  = 1000   →    8 - 1 = 0111
16 = 10000  →   16 - 1 = 01111
```

ANDing with that mask keeps the low bits and wipes the rest — exactly the remainder. The `assert(n > 0 && ((n - 1) & n) == 0)` in `h_init` isn't just safety; it's the contract that makes `hcode & mask` correct everywhere else. Real-world use you've already seen: `n & 1` to test "is odd."

### Intrusive data structures (and cache locality)

CPUs don't read RAM directly per access. They pull a chunk of nearby memory into a small fast cache next to the core. If the next thing you need is in that chunk, that's a **cache hit** — basically free. If not, **cache miss** — back to RAM, ~100× slower.

In a non-intrusive hashtable, the node and the payload are separate allocations at unrelated addresses. Walking a chain: fetch node → cache miss for payload → fetch payload. Two trips per hop.

In an intrusive hashtable (this project), the `HNode` is *embedded inside* the payload struct. Pulling the node into cache pulls the payload with it. One trip per hop. Multiplied across thousands of entries, the difference is real.

The price you pay is `container_of`. Given an `HNode*`, you recover the `Entry*` by subtracting the field's offset within the struct:

```c
#define container_of(ptr, type, member) \
    ((type *)( (char *)(ptr) - offsetof(type, member) ))
```

## C/C++ idioms

### Naming conventions used in this code

- `k_` → konstant (German *Konstante*) — compile-time constant, e.g. `k_max_msg`
- `g_` → global, e.g. `g_data`, `g_map`
- `sin_` in `sin_family`, `sin_port` → "sockaddr internet". C has no namespaces, so struct fields use prefixes to dodge collisions.
- `SOL_SOCKET` → "socket option level: socket" (i.e. configure the socket itself, not a higher protocol layer).

### `void` vs `void*`

- `void` = returns nothing.
- `void*` = a pointer to *something*, type unknown. The compiler knows it's an address; it doesn't know what's there. You must cast before dereferencing. `malloc()` returns `void*` for exactly this reason. The "void" in `void*` means "type unknown," **not** "no address."

### What a struct/class actually is in memory

A struct isn't a container with its own walls — it's a compile-time blueprint telling the compiler how to lay data members out contiguously. An empty struct is 1 byte (so distinct instances have distinct addresses), but that 1 byte vanishes the moment you add any real data. Non-virtual methods add **nothing** to instance size; they're functions in the code segment that take a hidden `this` pointer. The first virtual method adds 8 bytes for a vtable pointer; additional virtuals only grow the vtable, not the object.

I used to think classes existed to *hold* data. It's the other way around: data members are the thing, and the class is just the name of their arrangement.

### Header path conventions

- `sys/...` → kernel-facing interfaces (syscalls: `socket()`, `bind()`).
- non-`sys/` → userspace utilities, no syscalls. `arpa/inet.h` for `htons`, etc.
- `struct sockaddr_in` actually lives in `<netinet/in.h>` — it's available via `<arpa/inet.h>` only because that header includes it transitively.

### Subtle bugs to watch for

- `malloc()` doesn't run C++ constructors. Default member initializers like `int fd = -1` silently won't fire. Use `new` (or initialize every field explicitly after malloc — chapter 05's `accept_new_conn` does this).
- `&data` vs `data` when `data` is already a pointer: `&data` is the address of the pointer itself, sitting on the stack. Compiles fine, crashes at runtime.

### C++ build pipeline (background)

`.cpp` source → **preprocessor** (`#include` expansion, macros) → **compiler** (`.o` object file) → **linker** (resolves symbols across objects, produces executable) → **loader** (OS maps the executable into memory) → **runtime**.

- *Compile time* = decided before the program runs, baked into the binary.
- *Link time* = symbols resolved across object files. "undefined reference" errors live here.
- *Runtime* = decided while running (input, syscalls, network).

The preprocessor is a separate mini-language. `#include` literally copy-pastes the file. `g++ -E file.cpp` shows the expanded result — a tiny file becomes thousands of lines.

---
