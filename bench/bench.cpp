// Benchmark harness for the custom binary-protocol KV server.
// Protocol frame: [4B total-len][4B nargs][ (4B arglen + argbytes) ... ]
//   total-len covers everything after it (nargs field + all args).
// Response frame: [4B total-len][4B rescode][ data ]
//
// Modes:
//   pipeline  -- write a batch of P requests, then read P responses. Measures
//                server processing throughput (ops/sec ceiling).
//   sync      -- send one request, wait for its reply, repeat. Measures
//                request/response latency; reports p50/p99/p999.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <string>
#include <vector>
#include <algorithm>

static const size_t k_max_msg = 4096;

static void die(const char *m) { perror(m); exit(1); }

static double now_sec() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int dial() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) die("socket");
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)); // no Nagle
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr))) die("connect");
    return fd;
}

// Encode one command frame into buf, return bytes written.
static size_t encode(uint8_t *buf, const std::vector<std::string> &cmd) {
    uint32_t len = 4; // nargs field
    for (auto &s : cmd) len += 4 + s.size();
    uint32_t n = cmd.size();
    memcpy(buf, &len, 4);
    memcpy(buf + 4, &n, 4);
    size_t cur = 8;
    for (auto &s : cmd) {
        uint32_t p = s.size();
        memcpy(buf + cur, &p, 4);
        memcpy(buf + cur + 4, s.data(), s.size());
        cur += 4 + s.size();
    }
    return cur; // = 4 + len
}

static int32_t write_all(int fd, const uint8_t *buf, size_t n) {
    while (n > 0) {
        ssize_t rv = write(fd, buf, n);
        if (rv <= 0) return -1;
        n -= rv; buf += rv;
    }
    return 0;
}

// Read exactly one response frame off fd, using a persistent carry buffer.
// Returns 0 on success. Consumes leftover bytes across calls.
struct Reader {
    int fd;
    std::vector<uint8_t> buf;
    size_t off = 0;
    Reader(int f) : fd(f) { buf.reserve(1 << 20); }

    bool fill() {
        uint8_t tmp[65536];
        ssize_t rv = read(fd, tmp, sizeof(tmp));
        if (rv <= 0) return false;
        buf.insert(buf.end(), tmp, tmp + rv);
        return true;
    }
    // pull one frame; blocks (via read) until a full frame is buffered
    int32_t one() {
        for (;;) {
            if (buf.size() - off >= 4) {
                uint32_t len;
                memcpy(&len, buf.data() + off, 4);
                if (buf.size() - off >= 4 + len) {
                    off += 4 + len;
                    if (off > (1 << 20)) { // compact
                        buf.erase(buf.begin(), buf.begin() + off);
                        off = 0;
                    }
                    return 0;
                }
            }
            if (!fill()) return -1;
        }
    }
};

int main(int argc, char **argv) {
    // args: mode(pipeline|sync) cmd(set|get|del) total pipeline_depth
    std::string mode = argc > 1 ? argv[1] : "pipeline";
    std::string op   = argc > 2 ? argv[2] : "set";
    long total       = argc > 3 ? atol(argv[3]) : 1000000;
    int depth        = argc > 4 ? atoi(argv[4]) : 256;

    int fd = dial();

    // Pre-seed one key so GET/DEL hit something.
    {
        uint8_t b[512];
        std::vector<std::string> c = {"set", "k", "v"};
        size_t nb = encode(b, c);
        write_all(fd, b, nb);
        Reader r(fd); r.one();
    }

    auto make = [&](long i) -> std::vector<std::string> {
        std::string key = "key:" + std::to_string(i);
        if (op == "set") return {"set", key, "val:" + std::to_string(i)};
        if (op == "get") return {"get", key};
        if (op == "del") return {"del", key};
        return {"get", key};
    };

    Reader r(fd);
    double t0 = now_sec();

    if (mode == "sync") {
        std::vector<double> lat;
        lat.reserve(total);
        uint8_t b[4 + k_max_msg];
        for (long i = 0; i < total; i++) {
            auto c = make(i);
            size_t nb = encode(b, c);
            double s = now_sec();
            if (write_all(fd, b, nb)) die("write");
            if (r.one()) die("read");
            lat.push_back((now_sec() - s) * 1e6); // microseconds
        }
        double dt = now_sec() - t0;
        std::sort(lat.begin(), lat.end());
        auto pct = [&](double p) { return lat[(size_t)(p * (lat.size() - 1))]; };
        printf("%-8s %-4s sync      ops=%ld  %.0f ops/sec  "
               "p50=%.1fus p99=%.1fus p999=%.1fus\n",
               mode.c_str(), op.c_str(), total, total / dt,
               pct(0.50), pct(0.99), pct(0.999));
    } else {
        // pipeline: keep `depth` requests in flight.
        std::vector<uint8_t> batch;
        batch.reserve(depth * 64);
        long done = 0, sent = 0;
        uint8_t b[4 + k_max_msg];
        while (done < total) {
            batch.clear();
            int n = 0;
            while (n < depth && sent < total) {
                auto c = make(sent++);
                size_t nb = encode(b, c);
                batch.insert(batch.end(), b, b + nb);
                n++;
            }
            if (write_all(fd, batch.data(), batch.size())) die("write");
            for (int i = 0; i < n; i++) if (r.one()) die("read");
            done += n;
        }
        double dt = now_sec() - t0;
        printf("%-8s %-4s depth=%-4d ops=%ld  %.0f ops/sec  (%.2f s)\n",
               mode.c_str(), op.c_str(), depth, total, total / dt, dt);
    }

    close(fd);
    return 0;
}
