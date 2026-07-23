#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <assert.h>

const size_t k_max_msg = 4096;

static void die(const char *msg) {
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}

static int32_t read_full(int fd, char *buf, size_t n) {
    while (n > 0) {
        ssize_t rv = read(fd, buf, n);
        if (rv <= 0) return -1;
        assert((size_t)rv <= n);
        n -= (size_t)rv;
        buf += rv;
    }
    return 0;
}

static int32_t write_all(int fd, const char *buf, size_t n) {
    while (n > 0) {
        ssize_t rv = write(fd, buf, n);
        if (rv <= 0) return -1;
        assert((size_t)rv <= n);
        n -= (size_t)rv;
        buf += rv;
    }
    return 0;
}

static int32_t send_req(int fd, const char *text) {
    uint32_t len = (uint32_t)strlen(text);
    if (len > k_max_msg) return -1;

    char wbuf[4 + k_max_msg];
    memcpy(wbuf, &len, 4);
    memcpy(&wbuf[4], text, len);
    return write_all(fd, wbuf, 4 + len);
}

static int32_t read_res(int fd) {
    char rbuf[4 + k_max_msg + 1];
    errno = 0;
    
    // 1. Read Header
    int32_t err = read_full(fd, rbuf, 4);
    if (err) {
        if (errno == 0) fprintf(stderr, "EOF\n");
        else fprintf(stderr, "read() error\n");
        return err;
    }

    // 2. Read Body
    uint32_t len = 0;
    memcpy(&len, rbuf, 4);
    if (len > k_max_msg) {
        fprintf(stderr, "too long\n");
        return -1;
    }

    err = read_full(fd, &rbuf[4], len);
    if (err) {
        fprintf(stderr, "read() error\n");
        return err;
    }

    rbuf[4 + len] = '\0';
    printf("server says: %s\n", &rbuf[4]);
    return 0;
}

int main() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) die("socket()");

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int rv = connect(fd, (const struct sockaddr *)&addr, sizeof(addr));
    if (rv) die("connect");

    // PIPELINING TEST:
    // Send 3 requests back-to-back without waiting
    send_req(fd, "hello");
    send_req(fd, "world");
    send_req(fd, "pipelining");

    // Then read all 3 responses
    read_res(fd);
    read_res(fd);
    read_res(fd);

    close(fd);
    return 0;
}