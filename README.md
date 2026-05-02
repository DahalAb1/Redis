# Building Redis From Scratch

This is a ground-up implementation of a Redis-style key-value server in C++ — built without frameworks or abstractions in the way, working directly with sockets, the kernel, and bytes on the wire.

Each chapter introduces *one* problem and the smallest correct fix for it. Read in order, the working server emerges from `socket(2)` in five steps.

---

## The journey at a glance

| Chapter | What it adds | What it fixes |
|---------|--------------|---------------|
| [03_client_server](./03_client_server/) | TCP client + server using raw BSD sockets | Nothing — establishes the baseline. Quietly broken. |
| [04_protocol_client_server](./04_protocol_client_server/) | Length-prefix message protocol | TCP is a byte stream — `read()` may return partial messages |
| [06_event_polling](./06_event_polling/) | Single-threaded event loop with `poll()` + non-blocking I/O | Blocking I/O froze every other client during one slow `read()` |
| [07_get_sel_del](./07_get_sel_del/) | `GET` / `SET` / `DEL` over a structured request format | The server only echoed bytes — no concept of commands or keys |
| [08_hashtables](./08_hashtables/) | Intrusive hashtable with progressive resizing | `std::map` is a tree (O(log n)) and a one-shot rehash would stall the loop |

(Numbering skips 05 because that chapter — non-blocking mode — was folded directly into 06.)

---

## Final architecture (after chapter 08)

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

## Chapter 03 — Talking over TCP

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

## Chapter 04 — Framing messages

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

## Chapter 06 — One thread, many clients

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

## Chapter 07 — GET, SET, DEL

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

## Chapter 08 — A real hashtable

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
# build server (chapter 08 has the multi-file case)
cd 08_hashtables
g++ -Wall -O2 -std=c++17 -o server.out 08_server.cpp 08_hashtables.cpp
./server.out

# in another terminal — chapter 07's client speaks the same protocol
cd ../07_get_sel_del
g++ -Wall -O2 -std=c++17 -o client.out 07_client.cpp
./client.out set foo bar
./client.out get foo
./client.out del foo
```

Earlier chapters build with a single `g++ -o out file.cpp`.

---

## Repo map

```
03_client_server/             baseline TCP echo
04_protocol_client_server/    + length-prefix framing
06_event_polling/             + non-blocking + poll() event loop
07_get_sel_del/               + GET/SET/DEL over std::map
08_hashtables/                + custom intrusive hashtable
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

There are *no walls* between writes. The "stream" isn't a consequence of physical wires — packets on the wire have boundaries. It's a deliberate design choice: TCP's job is reliable, ordered byte delivery, not message framing. UDP preserves boundaries (each `recv()` is one datagram); TCP doesn't. Framing is your application's job. That's the whole reason Chapter 04 exists.

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

- `malloc()` doesn't run C++ constructors. Default member initializers like `int fd = -1` silently won't fire. Use `new` (or initialize every field explicitly after malloc — chapter 08's `accept_new_conn` does this).
- `&data` vs `data` when `data` is already a pointer: `&data` is the address of the pointer itself, sitting on the stack. Compiles fine, crashes at runtime.

### C++ build pipeline (background)

`.cpp` source → **preprocessor** (`#include` expansion, macros) → **compiler** (`.o` object file) → **linker** (resolves symbols across objects, produces executable) → **loader** (OS maps the executable into memory) → **runtime**.

- *Compile time* = decided before the program runs, baked into the binary.
- *Link time* = symbols resolved across object files. "undefined reference" errors live here.
- *Runtime* = decided while running (input, syscalls, network).

The preprocessor is a separate mini-language. `#include` literally copy-pastes the file. `g++ -E file.cpp` shows the expanded result — a tiny file becomes thousands of lines.

---
