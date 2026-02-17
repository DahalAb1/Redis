# 03 - Building a TCP Client & Server

## Motivation

Understanding how networked programs communicate at the lowest level — raw TCP sockets. This is the foundation everything else (HTTP, Redis protocol, databases) is built on.

## Background Concepts

- Another early name for the OS was the **supervisor** or even the **master control program**
- **Socket** — the doorway between the application layer (layer 7) and the transport layer (layer 4)
- **File descriptor (fd)** — an integer the OS uses to track open resources (files, sockets, pipes). The OS uses it to prevent direct access to its memory
- **`argc` / `argv`** — command-line argument count and values passed to `main()`

```cpp
// ./program hello 42
int main(int argc, char *argv[]) {
    // argc == 3
    // argv[0] == "./program"
    // argv[1] == "hello"
    // argv[2] == "42"
}
// argc = how many arguments
// argv = the arguments themselves
```

## TCP Connection Lifecycle

### 3-Way Handshake (open)
```
Client → SYN
Server → SYN + ACK
Client → ACK
```

### 4-Way Handshake (close)
```
Side A → FIN       (I'm done sending)
Side B → ACK       (got it, but I may still send)
Side B → FIN       (now I'm done too)
Side A → ACK       (final ack)
```
After the final ACK, Side A enters **TIME_WAIT**.

## Common Headers

| Header | Purpose |
|--------|---------|
| `<stdint.h>` | Fixed-width integer types (`uint8_t`, `uint16_t`, etc.) |
| `<stdlib.h>` | General utilities: `malloc`/`free`, `exit`, `abort` |
| `<string.h>` | String/memory ops: `memset`, `memcpy`, `strcmp` |
| `<stdio.h>` | Standard I/O: `printf`, `perror`, `fprintf` |
| `<errno.h>` | Error reporting via `errno` after system call failures |
| `<unistd.h>` | POSIX API: `close`, `read`, `write`, `fork` |
| `<arpa/inet.h>` | IP address conversion & byte order (`inet_pton`, `htons`, `ntohs`) |
| `<sys/socket.h>` | Core socket API: `socket`, `bind`, `listen`, `accept`, `connect` |


## Helper Functions

**`die()`** — prints error with `errno` and aborts the program
```cpp
static void die(const char *msg) {
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}
```
`errno` is a global variable set by system calls when they fail.

**`msg()`** — simple message printer to stderr
```cpp
static void msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
}
```

## Server Flow (5 steps)

### 1. Obtain a file descriptor (buy the phone)
```cpp
int fd = socket(AF_INET, SOCK_STREAM, 0);
```
- `AF_INET` = IPv4 (address family)
- `SOCK_STREAM` = TCP (reliable, ordered byte stream)
- `0` = default protocol for this domain + type

### 2. Configure the socket (the "reuse" hack)
```cpp
int val = 1;
setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
```
- `SOL_SOCKET` — setting option at the socket level, not protocol level
- `SO_REUSEADDR` — allows restarting the server immediately without waiting for OS to release the port (avoids TIME_WAIT blocking)

### 3. Bind (assign the phone number)
```cpp
struct sockaddr_in addr = {};
addr.sin_family = AF_INET;
addr.sin_port = htons(1234);       // port in network byte order
addr.sin_addr.s_addr = htonl(0);   // 0.0.0.0 = all interfaces
bind(fd, (const struct sockaddr *)&addr, sizeof(addr));
```
- `htons()` — host to network short (converts port to big-endian)
- `htonl()` — host to network long (converts IP to big-endian)
- Network always uses **big-endian**; your CPU might use little-endian

### 4. Listen (turn the ringer on)
```cpp
listen(fd, SOMAXCONN);
```
- `SOMAXCONN` — max queue size for pending connections (OS-defined)

### 5. Accept loop (the receptionist)
```cpp
while (true) {
    struct sockaddr_in client_addr = {};
    socklen_t socklen = sizeof(client_addr);
    int connfd = accept(fd, (struct sockaddr *)&client_addr, &socklen);
    if (connfd < 0) { continue; }
    do_something(connfd);
    close(connfd);
}
```
- `accept()` **blocks** until a client connects, returns a new fd (`connfd`) for that client
- The original `fd` keeps listening
- `connfd` is used to read/write with the connected client

## Client Flow (5 steps)

1. **Create socket** — same as server
2. **Connect** — `connect(fd, &addr, sizeof(addr))` initiates the 3-way handshake
   - `INADDR_LOOPBACK` = `127.0.0.1` (localhost)
3. **Send** — `write(fd, msg, strlen(msg))`
4. **Receive** — `read(fd, rbuf, sizeof(rbuf))`
5. **Close** — `close(fd)`

## Key Differences: Server vs Client

| | Server | Client |
|--|--------|--------|
| Flow | socket → setsockopt → bind → listen → accept → read/write → close | socket → connect → write → read → close |
| Binds to | `0.0.0.0` (all interfaces) | Connects to `127.0.0.1` (localhost) |
| File descriptors | **Two**: `fd` (listening) + `connfd` (per-client) | **One**: `fd` (the connection) |

## Deep Dive: TCP is a Byte Stream

TCP is a **stream protocol**, not a message protocol. `read()` might not return the entire message in one call. This code assumes it does — which is technically **broken**.

### What does "byte stream" mean?

When you call `write(fd, "hello", 5)`, you hand 5 bytes to the kernel. The kernel doesn't immediately send `"hello"` as-is. It:
- Puts those bytes into a **send buffer**
- Decides *when* and *how much* to send based on network conditions (packet size, congestion, etc.)
- Might chop it across multiple TCP packets, or bundle it with other data

On the receiving side, incoming packets arrive and the kernel dumps their payloads into a **receive buffer** — just a flat sequence of bytes. `read()` scoops bytes out of that buffer.

```
Client sends:           write("hello")  then  write("world")
                              |                     |
Kernel send buffer:     [h][e][l][l][o][w][o][r][l][d]
                              | (split into packets however TCP wants)
Kernel recv buffer:     [h][e][l][l][o][w][o][r][l][d]
                              |
Server reads:           read() might return "hellow"  then  read() returns "orld"
```

There are **no walls** between your two writes. It's like water through a hose — if you pour a red cup and then a blue cup in, on the other end you just get water. You can't tell where one ended and the other started.

### Why is it a stream? Is it because of physical wires?

No. The physical/network layer sends discrete **packets** with clear boundaries. A packet arrives whole. The "stream" behavior is a **deliberate software design choice** by TCP in the kernel.

```
Wire:        [packet1: "hel"] [packet2: "lo"] [packet3: "world"]
                    |               |               |
TCP kernel:  merges into →  [h][e][l][l][o][w][o][r][l][d]
                    |
Your code:   read() → whatever's available
```

TCP *could* have preserved packet boundaries (UDP does — each `recv()` gives you exactly one datagram). TCP chose not to, because its job is **reliable, ordered byte delivery**, not message framing. It reorders out-of-order packets, retransmits lost ones, and merges everything into one flat buffer.

Message framing is **your application's job** — which is why we need a protocol (covered in [04_protocol_client_server](../04_protocol_client_server/)).

## Deep Dive: Byte Order (Endianness)

### The problem

The number `0x01020304` can be stored in memory two ways:

| | Address 0 | Address 1 | Address 2 | Address 3 |
|--|-----------|-----------|-----------|-----------|
| **Big-endian** | `01` | `02` | `03` | `04` | Most significant byte first |
| **Little-endian** | `04` | `03` | `02` | `01` | Least significant byte first |

Different CPUs use different byte orders. If a little-endian machine sends raw bytes to a big-endian machine, multi-byte numbers get scrambled.

### The solution

The internet standardized on **big-endian** (aka "network byte order"). The conversion functions:

- `htons()` — **h**ost **to** **n**etwork **s**hort (16-bit, for ports)
- `htonl()` — **h**ost **to** **n**etwork **l**ong (32-bit, for IPs)
- `ntohs()` / `ntohl()` — the reverse

These are **smart** — on a big-endian machine they're no-ops, on little-endian they swap the bytes. You always call them and let the system figure it out.

## Files

- `03_server.cpp` — TCP server
- `03_client.cpp` — TCP client
- `Part 1.txt` — Raw learning notes

## Build & Run

```bash
# Terminal 1
g++ -o server.out 03_server.cpp && ./server.out

# Terminal 2
g++ -o client.out 03_client.cpp && ./client.out
```
