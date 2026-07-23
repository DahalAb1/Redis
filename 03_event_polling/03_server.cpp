#include <stdint.h>     // uint8_t, uint32_t — fixed-width integer types for protocol buffers
#include <assert.h>     // assert() — runtime sanity checks (e.g. buffer bounds)
#include <stdlib.h>     // malloc(), free(), abort() — dynamic memory and fatal exit
#include <string.h>     // memcpy(), memmove() — copying/shifting bytes in read/write buffers
#include <stdio.h>      // printf(), fprintf() — logging messages to stdout/stderr
#include <errno.h>      // errno, EAGAIN, EINTR — checking syscall error codes
#include <fcntl.h>      // fcntl(), F_GETFL, F_SETFL, O_NONBLOCK — setting fds to non-blocking mode
#include <poll.h>       // poll(), struct pollfd, POLLIN, POLLOUT — event loop I/O multiplexing
#include <unistd.h>     // read(), write(), close() — low-level fd I/O and cleanup
#include <arpa/inet.h>  // ntohs(), ntohl() — host/network byte order conversions
#include <sys/socket.h> // socket(), bind(), listen(), accept(), setsockopt() — socket lifecycle
#include <vector>       // std::vector — dynamic array for fd2conn map and poll_args list

// --- Helpers ---
static void msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
}

static void die(const char *msg) {
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}

// Set a file descriptor to non-blocking mode (Chapter 5)
static void fd_set_nb(int fd) {
    errno = 0;

    int flags = fcntl(fd, F_GETFL, 0);
    if (errno) {
        die("fcntl error");
        return;
    }

    flags |= O_NONBLOCK;

    
    if (fcntl(fd, F_SETFL, flags) == -1) {
      die("fcntl error");
    }

}

// --- Connection Data ---
const size_t k_max_msg = 4096;

enum {
    STATE_REQ = 0,
    STATE_RES = 1,
    STATE_END = 2,  // Mark for deletion
};

struct Conn {
    int fd = -1;
    uint32_t state = 0;     // STATE_REQ or STATE_RES
    
    // Read buffer
    size_t rbuf_size = 0;
    uint8_t rbuf[4 + k_max_msg] = {};

    // Write buffer
    size_t wbuf_size = 0;
    size_t wbuf_sent = 0;
    uint8_t wbuf[4 + k_max_msg] = {};
};

// --- State Machine Prototypes ---
static void state_req(Conn *conn);
static void state_res(Conn *conn);

// --- State Machine Implementation ---

static bool try_one_request(Conn *conn) {
    // 1. Try to parse a 4-byte header
    if (conn->rbuf_size < 4) {
        return false; // Not enough data
    }
    
    uint32_t len = 0;
    memcpy(&len, &conn->rbuf, 4);
    if (len > k_max_msg) {
        msg("too long");
        conn->state = STATE_END;
        return false;
    }
    
    // 2. Try to parse the body
    if (4 + len > conn->rbuf_size) {
        return false; // Not enough data
    }
    
    // 3. Got a full message! Do work.
    printf("client says: %.*s\n", len, &conn->rbuf[4]);
    
    // 4. Generate response (Echo)
    memcpy(&conn->wbuf, &len, 4);
    memcpy(&conn->wbuf[4], &conn->rbuf[4], len);
    conn->wbuf_size = 4 + len;
    
    // 5. Remove processed message from rbuf (Pipelining)
    size_t remain = conn->rbuf_size - 4 - len;
    if (remain) {
        memmove(conn->rbuf, &conn->rbuf[4 + len], remain);
    }
    conn->rbuf_size = remain;
    
    // 6. Switch state to Writing
    conn->state = STATE_RES;
    state_res(conn);
    
    // Continue outer loop if we finished processing this request
    return (conn->state == STATE_REQ);
}

static bool try_fill_buffer(Conn *conn) {
    assert(conn->rbuf_size < sizeof(conn->rbuf));
    
    ssize_t rv = 0;
    do {
        size_t cap = sizeof(conn->rbuf) - conn->rbuf_size;
        rv = read(conn->fd, &conn->rbuf[conn->rbuf_size], cap);
    } while (rv < 0 && errno == EINTR); // Retry on interrupt
    
    if (rv < 0 && errno == EAGAIN) {
        return false; // No data, stop
    }
    
    if (rv < 0) {
        msg("read() error");
        conn->state = STATE_END;
        return false;
    }
    
    if (rv == 0) {
        if (conn->rbuf_size > 0) {
            msg("unexpected EOF");
        } else {
            msg("EOF");
        }
        conn->state = STATE_END;
        return false;
    }
    
    conn->rbuf_size += (size_t)rv;
    assert(conn->rbuf_size <= sizeof(conn->rbuf));
    
    // Process requests while we have data
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
    
    if (rv < 0 && errno == EAGAIN) {
        return false; // Socket buffer full, stop
    }
    
    if (rv < 0) {
        msg("write() error");
        conn->state = STATE_END;
        return false;
    }
    
    conn->wbuf_sent += (size_t)rv;
    assert(conn->wbuf_sent <= conn->wbuf_size);
    
    if (conn->wbuf_sent == conn->wbuf_size) {
        // Fully sent! Switch back to reading
        conn->state = STATE_REQ;
        conn->wbuf_sent = 0;
        conn->wbuf_size = 0;
        return false;
    }
    
    // Still have data to send
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
        assert(0); // Should not happen
    }
}

// --- Main Loop Infrastructure ---

static void conn_put(std::vector<Conn *> &fd2conn, struct Conn *conn) {
    if (fd2conn.size() <= (size_t)conn->fd) {
        fd2conn.resize(conn->fd + 1);
    }
    fd2conn[conn->fd] = conn;
}

static int32_t accept_new_conn(std::vector<Conn *> &fd2conn, int fd) {
    // 1. Accept
    struct sockaddr_in client_addr = {};
    socklen_t socklen = sizeof(client_addr);
    int connfd = accept(fd, (struct sockaddr *)&client_addr, &socklen);
    if (connfd < 0) {
        msg("accept() error");
        return -1;
    }
    
    // 2. Set Non-blocking (Crucial!)
    fd_set_nb(connfd);
    
    // 3. Create Conn struct
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
    // 1. Setup Listening Socket
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { die("socket()"); }
    
    int val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
    
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr = htonl(0);
    
    int rv = bind(fd, (const sockaddr *)&addr, sizeof(addr));
    if (rv) { die("bind()"); }
    
    rv = listen(fd, SOMAXCONN);
    if (rv) { die("listen()"); }
    
    // 2. Set Listener to Non-blocking
    fd_set_nb(fd);
    
    // 3. The Event Loop
    std::vector<Conn *> fd2conn;
    std::vector<struct pollfd> poll_args;
    
    while (true) {
        // Prepare poll arguments
        poll_args.clear();
        
        // A. Add the listener
        struct pollfd pfd = {fd, POLLIN, 0};
        poll_args.push_back(pfd);
        
        // B. Add the clients
        for (Conn *conn : fd2conn) {
            if (!conn) continue;
           struct pollfd pfd = {};
            pfd.fd = conn->fd;
            pfd.events = (conn->state == STATE_REQ) ? POLLIN : POLLOUT;
            pfd.events = pfd.events | POLLERR;
            poll_args.push_back(pfd);
        } 
        
        // C. Poll (Blocks here!)
        int rv = poll(poll_args.data(), (nfds_t)poll_args.size(), 1000);
        if (rv < 0) { die("poll"); }
        
        // D. Handle Clients
        for (size_t i = 1; i < poll_args.size(); ++i) {
            if (poll_args[i].revents) {
                Conn *conn = fd2conn[poll_args[i].fd];
                connection_io(conn);




                if (conn->state == STATE_END) {
                    // Destroy connection
                    fd2conn[conn->fd] = NULL;
                    (void)close(conn->fd);
                    delete conn;
                }
            }
        }
        
        // E. Handle New Connections
        if (poll_args[0].revents) {
            (void)accept_new_conn(fd2conn, fd);
        }
    }
    
    return 0;
}


