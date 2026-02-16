# 04 - Protocol: Client & Server

## Motivation

In [03_client_server](../03_client_server/), we built a basic TCP client and server. But that code was **technically broken** — it assumed `read()` would return the entire message in one call.

TCP is a **byte stream**, not a message stream. The kernel buffers incoming data and `read()` returns however many bytes are available — could be partial, could be multiple messages merged together. There are no message boundaries.

So the question becomes: **how does the receiver know where one message ends and the next begins?**

The answer: you define a **protocol**.

## The Protocol

This project uses the simplest possible protocol:

```
[4-byte length header][payload]
```

- First, read exactly 4 bytes — this tells you how long the payload is
- Then, read exactly that many bytes — that's your complete message
- Replies follow the same format

This is called a **length-prefixed** or **TLV (Type-Length-Value)** style protocol (without the Type part).

## Key Learnings

### 1. `read()` and `write()` are unreliable for exact amounts

A single `read()` might return fewer bytes than requested. A single `write()` might not send everything. You need helper functions that **loop until all bytes are transferred**:

```cpp
// read_full()  — loops read() until exactly n bytes are read
// write_all()  — loops write() until exactly n bytes are written
```

### 2. TCP stream vs messages

- TCP gives you a **stream of bytes** — no boundaries, no structure
- UDP gives you **datagrams** — each `recv()` returns exactly one send
- TCP chose streams because its job is reliable, ordered delivery — not message framing
- Message framing is **your application's job**

### 3. The protocol enables multiple requests per connection

With the length-prefix protocol, the server can now loop and handle **multiple requests** on the same connection:

```cpp
while (true) {
    int32_t err = one_request(connfd);
    if (err) break;
}
```

Without a protocol, the server wouldn't know when one request ends and the next starts.

### 4. Byte order caveat

The code uses `memcpy(&len, rbuf, 4)` and assumes little-endian for the protocol header. This works when client and server are on the same machine, but for a proper protocol you should use `htonl()`/`ntohl()` to convert the length header to/from network byte order (big-endian) — just like we do for `sin_port` and `sin_addr`.

## Files

- `04_server.cpp` — Server that reads length-prefixed messages and replies
- `04_client.cpp` — Client that sends length-prefixed messages

## Build & Run

```bash
# Terminal 1: Start server
g++ -o server.out 04_server.cpp && ./server.out

# Terminal 2: Run client
g++ -o client.out 04_client.cpp && ./client.out
```
