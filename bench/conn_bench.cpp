// Concurrency benchmark for the single-threaded, poll()-based KV server.
//
// bench.cpp measures throughput over ONE connection. That says nothing about
// the concurrency claim. This harness tests the three things that claim asserts:
//
//   fanout N ops   -- open N concurrent connections, keep 1 request in flight on
//                     each, drive them all from a single client-side poll loop.
//                     Answers: can the server actually hold N sockets at once,
//                     and what does aggregate throughput / per-request latency
//                     look like as N grows?
//
//   idle N ops     -- open N connections that connect and then say nothing, then
//                     run one active client in strict request/response mode.
//                     Answers: do idle connections slow down an active one?
//
//   slow N ops     -- open N connections that blast requests and then NEVER read
//                     their replies (so the server's per-connection write buffer
//                     fills and those connections wedge in STATE_RES), then run
//                     one active client. Answers: does a stuck client block
//                     everyone else?
//
// Protocol frame: [4B total-len][4B nargs][ (4B arglen + argbytes) ... ]
//   total-len covers everything after it. Response: [4B total-len][4B rescode][data]
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <string>
#include <vector>
#include <algorithm>

static void die(const char *m) { perror(m); exit(1); }

static double now_sec() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void set_nb(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Blocking connect to 127.0.0.1:1234. Retries on ECONNREFUSED, which is what a
// full listen backlog looks like to the client (kern.ipc.somaxconn is 128 on
// macOS and the server accepts one connection per poll() iteration).
static int dial(int retries = 200) {
    for (int attempt = 0; ; attempt++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        struct sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(1234);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (0 == connect(fd, (struct sockaddr *)&addr, sizeof(addr))) return fd;
        close(fd);
        if (attempt >= retries) return -1;
        struct timespec ts = {0, 200 * 1000};  // 200us backoff
        nanosleep(&ts, NULL);
    }
}

static size_t encode(uint8_t *buf, const std::vector<std::string> &cmd) {
    uint32_t len = 4;
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
    return cur;
}

static int32_t write_all(int fd, const uint8_t *buf, size_t n) {
    while (n > 0) {
        ssize_t rv = write(fd, buf, n);
        if (rv <= 0) {
            if (rv < 0 && (errno == EINTR)) continue;
            return -1;
        }
        n -= rv; buf += rv;
    }
    return 0;
}

// Blocking reader for exactly one response frame, with carry-over buffer.
struct SyncReader {
    int fd;
    std::vector<uint8_t> buf;
    size_t off = 0;
    SyncReader(int f) : fd(f) {}
    int32_t one() {
        for (;;) {
            if (buf.size() - off >= 4) {
                uint32_t len;
                memcpy(&len, buf.data() + off, 4);
                if (buf.size() - off >= 4 + len) {
                    off += 4 + len;
                    if (off > (1 << 16)) { buf.erase(buf.begin(), buf.begin() + off); off = 0; }
                    return 0;
                }
            }
            uint8_t tmp[65536];
            ssize_t rv = read(fd, tmp, sizeof(tmp));
            if (rv <= 0) return -1;
            buf.insert(buf.end(), tmp, tmp + rv);
        }
    }
};

static void report_latency(const char *label, std::vector<double> &lat, double dt, long ops) {
    std::sort(lat.begin(), lat.end());
    auto pct = [&](double p) { return lat[(size_t)(p * (lat.size() - 1))]; };
    printf("%-28s ops=%-8ld %8.0f ops/sec   p50=%7.1fus p99=%8.1fus p999=%8.1fus max=%9.1fus\n",
           label, ops, ops / dt, pct(0.50), pct(0.99), pct(0.999), lat.back());
}

// ---- fanout: N concurrent connections, 1 request in flight each ----------
struct FanConn {
    int fd = -1;
    std::vector<uint8_t> rbuf;   // partial response bytes
    double sent_at = 0;
    bool awaiting = false;
};

static void run_fanout(int nconn, long total_ops) {
    std::vector<FanConn> conns(nconn);
    double t_dial = now_sec();
    int established = 0;
    for (int i = 0; i < nconn; i++) {
        conns[i].fd = dial();
        if (conns[i].fd < 0) break;
        set_nb(conns[i].fd);
        established++;
    }
    double dial_dt = now_sec() - t_dial;
    if (established == 0) { fprintf(stderr, "no connections established\n"); exit(1); }
    conns.resize(established);
    printf("  established %d/%d connections in %.2fs (%.0f conn/sec)\n",
           established, nconn, dial_dt, established / dial_dt);

    std::vector<double> lat;
    lat.reserve(total_ops);
    long done = 0, issued = 0;
    uint8_t b[512];
    std::vector<struct pollfd> pfds(established);

    double t0 = now_sec();
    while (done < total_ops) {
        // issue a request on every idle connection
        for (int i = 0; i < established; i++) {
            if (conns[i].awaiting || issued >= total_ops) continue;
            std::vector<std::string> c = {"get", "key:" + std::to_string(issued)};
            size_t nb = encode(b, c);
            conns[i].sent_at = now_sec();
            if (write_all(conns[i].fd, b, nb)) die("write");
            conns[i].awaiting = true;
            issued++;
        }
        for (int i = 0; i < established; i++) {
            pfds[i].fd = conns[i].fd;
            pfds[i].events = conns[i].awaiting ? POLLIN : 0;
            pfds[i].revents = 0;
        }
        int rv = poll(pfds.data(), (nfds_t)established, 5000);
        if (rv < 0) { if (errno == EINTR) continue; die("poll"); }
        if (rv == 0) { fprintf(stderr, "client poll timeout, done=%ld\n", done); break; }

        for (int i = 0; i < established && done < total_ops; i++) {
            if (!(pfds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            uint8_t tmp[8192];
            ssize_t n = read(conns[i].fd, tmp, sizeof(tmp));
            if (n <= 0) { if (errno == EAGAIN) continue; die("read"); }
            FanConn &c = conns[i];
            c.rbuf.insert(c.rbuf.end(), tmp, tmp + n);
            // consume whole frames
            size_t off = 0;
            while (c.rbuf.size() - off >= 4) {
                uint32_t len;
                memcpy(&len, c.rbuf.data() + off, 4);
                if (c.rbuf.size() - off < 4 + len) break;
                off += 4 + len;
                lat.push_back((now_sec() - c.sent_at) * 1e6);
                c.awaiting = false;
                done++;
            }
            if (off) c.rbuf.erase(c.rbuf.begin(), c.rbuf.begin() + off);
        }
    }
    double dt = now_sec() - t0;
    char label[64];
    snprintf(label, sizeof(label), "fanout conns=%d", established);
    report_latency(label, lat, dt, done);
    for (auto &c : conns) close(c.fd);
}

// ---- background connections: idle (silent) or slow (write, never read) ----
static std::vector<int> open_background(int n, bool slow) {
    std::vector<int> fds;
    fds.reserve(n);
    for (int i = 0; i < n; i++) {
        int fd = dial();
        if (fd < 0) break;
        fds.push_back(fd);
    }
    if (slow) {
        // Blast requests without ever reading a reply. The server's 4KB
        // per-connection write buffer fills, write() returns EAGAIN, and the
        // connection wedges in STATE_RES until the client drains -- which it
        // never does.
        uint8_t b[512];
        for (int fd : fds) {
            set_nb(fd);
            std::vector<uint8_t> blast;
            for (int k = 0; k < 4000; k++) {
                std::vector<std::string> c = {"get", "key:" + std::to_string(k)};
                size_t nb = encode(b, c);
                blast.insert(blast.end(), b, b + nb);
            }
            size_t off = 0;
            while (off < blast.size()) {           // write until the kernel says stop
                ssize_t rv = write(fd, blast.data() + off, blast.size() - off);
                if (rv <= 0) break;                // EAGAIN => send buffer full, done
                off += rv;
            }
        }
    }
    return fds;
}

// ---- one active client in strict request/response mode --------------------
static void run_active(const char *label, long ops) {
    int fd = dial();
    if (fd < 0) die("dial active");
    SyncReader r(fd);
    uint8_t b[512];
    // pre-seed so GET hits
    { std::vector<std::string> c = {"set", "k", "v"}; write_all(fd, b, encode(b, c)); r.one(); }

    std::vector<double> lat;
    lat.reserve(ops);
    double t0 = now_sec();
    for (long i = 0; i < ops; i++) {
        std::vector<std::string> c = {"get", "k"};
        size_t nb = encode(b, c);
        double s = now_sec();
        if (write_all(fd, b, nb)) die("write");
        if (r.one()) die("read");
        lat.push_back((now_sec() - s) * 1e6);
    }
    double dt = now_sec() - t0;
    report_latency(label, lat, dt, ops);
    close(fd);
}

int main(int argc, char **argv) {
    std::string mode = argc > 1 ? argv[1] : "fanout";
    int n            = argc > 2 ? atoi(argv[2]) : 1000;
    long ops         = argc > 3 ? atol(argv[3]) : 100000;

    if (mode == "fanout") {
        run_fanout(n, ops);
    } else if (mode == "idle" || mode == "slow") {
        bool slow = (mode == "slow");
        double t0 = now_sec();
        std::vector<int> bg = open_background(n, slow);
        printf("  %s background connections: %zu/%d established in %.2fs\n",
               mode.c_str(), bg.size(), n, now_sec() - t0);
        char label[64];
        snprintf(label, sizeof(label), "active w/ %zu %s", bg.size(), mode.c_str());
        run_active(label, ops);
        for (int fd : bg) close(fd);
    } else {
        fprintf(stderr, "usage: %s fanout|idle|slow N ops\n", argv[0]);
        return 1;
    }
    return 0;
}
