#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <strings.h>
#include <string>
#include <vector>
#include <map>

// --- Helper Functions ---
static void msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
}

static void die(const char *msg) {
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}

static void fd_set_nb(int fd) {
    errno = 0;
    int flags = fcntl(fd, F_GETFL, 0);
    if (errno) {
        die("fcntl error");
        return;
    }
    flags |= O_NONBLOCK;
    errno = 0;
    (void)fcntl(fd, F_SETFL, flags);
    if (errno) {
        die("fcntl error");
    }
}

// --- Global Data Store & Enums ---
const size_t k_max_msg = 4096;
const size_t k_max_args = 1024; // Safety limit for number of strings

enum {
    STATE_REQ = 0,
    STATE_RES = 1,
    STATE_END = 2,
};

// Response codes [4]
enum {
    RES_OK = 0,
    RES_ERR = 1,
    RES_NX = 2,
};

// The data structure for the key space. This is just a placeholder
// until we implement a custom hashtable in the next chapter [4].
static std::map<std::string, std::string> g_map;

struct Conn {
    int fd = -1;
    uint32_t state = 0;
    size_t rbuf_size = 0;
    uint8_t rbuf[4 + k_max_msg] = {};
    size_t wbuf_size = 0;
    size_t wbuf_sent = 0;
    uint8_t wbuf[4 + k_max_msg] = {};
};

static void state_req(Conn *conn);
static void state_res(Conn *conn);

// --- Command Execution Logic [4, 5] ---

static uint32_t do_get(
    const std::vector<std::string> &cmd, uint8_t *res, uint32_t *reslen)
{
    if (!g_map.count(cmd[1])) {
        return RES_NX;
    }
    std::string &val = g_map[cmd[1]];
    assert(val.size() <= k_max_msg);
    memcpy(res, val.data(), val.size());
    *reslen = (uint32_t)val.size();
    return RES_OK;
}

static uint32_t do_set(
    const std::vector<std::string> &cmd, uint8_t *res, uint32_t *reslen)
{
    (void)res;
    (void)reslen;
    g_map[cmd[1]] = cmd[2];
    return RES_OK;
}

static uint32_t do_del(
    const std::vector<std::string> &cmd, uint8_t *res, uint32_t *reslen)
{
    (void)res;
    (void)reslen;
    g_map.erase(cmd[1]);
    return RES_OK;
}

// --- Protocol Parsing & Routing [3, 8] ---

static bool cmd_is(const std::string &word, const char *cmd) {
    return 0 == strcasecmp(word.c_str(), cmd);
}

// Splits the raw byte buffer into a list of strings [8]
static int32_t parse_req(
    const uint8_t *data, size_t len, std::vector<std::string> &out)
{
    if (len < 4) return -1;
    uint32_t n = 0;
    memcpy(&n, data, 4);
    if (n > k_max_args) return -1;
    
    size_t pos = 4;
    while (n--) {
        if (pos + 4 > len) return -1;
        uint32_t sz = 0;
        memcpy(&sz, &data[pos], 4);
        if (pos + 4 + sz > len) return -1;
        
        out.push_back(std::string((char *)&data[pos + 4], sz));
        pos += 4 + sz;
    }
    if (pos != len) return -1;  // trailing garbage check
    return 0;
}

// Routes the parsed strings to the correct command function [3, 9]
static int32_t do_request(
    const uint8_t *req, uint32_t reqlen,
    uint32_t *rescode, uint8_t *res, uint32_t *reslen)
{
    std::vector<std::string> cmd;
    if (0 != parse_req(req, reqlen, cmd)) {
        msg("bad req");
        return -1;
    }

    if (cmd.size() == 2 && cmd_is(cmd[0], "get")) {
        *rescode = do_get(cmd, res, reslen);
    } else if (cmd.size() == 3 && cmd_is(cmd[0], "set")) {
        *rescode = do_set(cmd, res, reslen);
    } else if (cmd.size() == 2 && cmd_is(cmd[0], "del")) {
        *rescode = do_del(cmd, res, reslen);
    } else {
        *rescode = RES_ERR;
        const char *err_msg = "Unknown cmd";
        strcpy((char *)res, err_msg);
        *reslen = strlen(err_msg);
        return 0;
    }
    return 0;
}

// --- Event Loop State Machine [2, 10, 11] ---

static bool try_one_request(Conn *conn) {
    // 1. Read Length Header
    if (conn->rbuf_size < 4) {
        return false;
    }
    uint32_t len = 0;
    memcpy(&len, &conn->rbuf, 4);
    if (len > k_max_msg) {
        msg("too long");
        conn->state = STATE_END;
        return false;
    }
    if (4 + len > conn->rbuf_size) {
        return false; 
    }

    // 2. Process Request [10]
    uint32_t rescode = 0;
    uint32_t wlen = 0;
    int32_t err = do_request(
        &conn->rbuf[4], len,
        &rescode, &conn->wbuf[4 + 4], &wlen
    );
    if (err) {
        conn->state = STATE_END;
        return false;
    }

    // 3. Pack the Response [11]
    wlen += 4;
    memcpy(&conn->wbuf, &wlen, 4);
    memcpy(&conn->wbuf[4], &rescode, 4);
    conn->wbuf_size = 4 + wlen;

    // 4. Shift buffer (Pipelining support) [11]
    size_t remain = conn->rbuf_size - 4 - len;
    if (remain) {
        memmove(conn->rbuf, &conn->rbuf[4 + len], remain);
    }
    conn->rbuf_size = remain;

    conn->state = STATE_RES;
    state_res(conn);
    return (conn->state == STATE_REQ);
}

// (The remaining functions are identical to the Chapter 6 Event Loop)
static bool try_fill_buffer(Conn *conn) {
    assert(conn->rbuf_size < sizeof(conn->rbuf));
    ssize_t rv = 0;
    do {
        size_t cap = sizeof(conn->rbuf) - conn->rbuf_size;
        rv = read(conn->fd, &conn->rbuf[conn->rbuf_size], cap);
    } while (rv < 0 && errno == EINTR);
    
    if (rv < 0 && errno == EAGAIN) return false;
    if (rv < 0) {
        msg("read() error");
        conn->state = STATE_END;
        return false;
    }
    if (rv == 0) {
        if (conn->rbuf_size > 0) msg("unexpected EOF");
        else msg("EOF");
        conn->state = STATE_END;
        return false;
    }
    
    conn->rbuf_size += (size_t)rv;
    while (try_one_request(conn)) {}
    return (conn->state == STATE_REQ);
}

static void state_req(Conn *conn) {
    while (try_fill_buffer(conn)) {}
}

static bool try_flush_buffer(Conn *conn) {
    ssize_t rv = 0;
    do {
        size_t remain = conn->wbuf_size - conn->wbuf_sent;
        rv = write(conn->fd, &conn->wbuf[conn->wbuf_sent], remain);
    } while (rv < 0 && errno == EINTR);
    
    if (rv < 0 && errno == EAGAIN) return false;
    if (rv < 0) {
        msg("write() error");
        conn->state = STATE_END;
        return false;
    }
    
    conn->wbuf_sent += (size_t)rv;
    if (conn->wbuf_sent == conn->wbuf_size) {
        conn->state = STATE_REQ;
        conn->wbuf_sent = 0;
        conn->wbuf_size = 0;
        return false;
    }
    return true;
}

static void state_res(Conn *conn) {
    while (try_flush_buffer(conn)) {}
}

static void connection_io(Conn *conn) {
    if (conn->state == STATE_REQ) {
        state_req(conn);
    } else if (conn->state == STATE_RES) {
        state_res(conn);
    } else {
        assert(0);
    }
}

static void conn_put(std::vector<Conn *> &fd2conn, struct Conn *conn) {
    if (fd2conn.size() <= (size_t)conn->fd) {
        fd2conn.resize(conn->fd + 1);
    }
    fd2conn[conn->fd] = conn;
}

static int32_t accept_new_conn(std::vector<Conn *> &fd2conn, int fd) {
    struct sockaddr_in client_addr = {};
    socklen_t socklen = sizeof(client_addr);
    int connfd = accept(fd, (struct sockaddr *)&client_addr, &socklen);
    if (connfd < 0) {
        msg("accept() error");
        return -1;
    }
    fd_set_nb(connfd);
    struct Conn *conn = new (std::nothrow) Conn();
    if (!conn) {
        close(connfd);
        return -1;
    }
    conn->fd = connfd;
    conn_put(fd2conn, conn);
    return 0;
}

int main() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) die("socket()");
    
    int val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
    
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr = htonl(0);
    
    int rv = bind(fd, (const sockaddr *)&addr, sizeof(addr));
    if (rv) die("bind()");
    
    rv = listen(fd, SOMAXCONN);
    if (rv) die("listen()");
    
    fd_set_nb(fd);
    
    std::vector<Conn *> fd2conn;
    std::vector<struct pollfd> poll_args;
    
    while (true) {
        poll_args.clear();
        struct pollfd pfd = {fd, POLLIN, 0};
        poll_args.push_back(pfd);
        
        for (Conn *conn : fd2conn) {
            if (!conn) continue;
            struct pollfd pfd = {};
            pfd.fd = conn->fd;
            pfd.events = (conn->state == STATE_REQ) ? POLLIN : POLLOUT;
            pfd.events = pfd.events | POLLERR;
            poll_args.push_back(pfd);
        }
        
        int rv = poll(poll_args.data(), (nfds_t)poll_args.size(), 1000);
        if (rv < 0) die("poll");
        
        for (size_t i = 1; i < poll_args.size(); ++i) {
            if (poll_args[i].revents) {
                Conn *conn = fd2conn[poll_args[i].fd];
                connection_io(conn);
                if (conn->state == STATE_END) {
                    fd2conn[conn->fd] = NULL;
                    (void)close(conn->fd);
                    delete conn;
                }
            }
        }
        
        if (poll_args[0].revents) {
            (void)accept_new_conn(fd2conn, fd);
        }
    }
    return 0;
}