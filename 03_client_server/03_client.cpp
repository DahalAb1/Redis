#include <stdlib.h> 
#include <string.h> 
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

// Helper function to print errors and exit
static void die(const char *msg) {
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}

int main() {
    // 1. Create a socket (The Phone)
    // AF_INET = IPv4, SOCK_STREAM = TCP
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        die("socket()");
    }

    // 2. Connect to the server (The Dialing)
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);     // Port 1234
    // INADDR_LOOPBACK is 127.0.0.1 (localhost)
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int rv = connect(fd, (const struct sockaddr *)&addr, sizeof(addr));
    if (rv) {
        die("connect");
    }

    // 3. Send a message
    char msg[] = "hello";
    write(fd, msg, strlen(msg));

    // 4. Read the response
    char rbuf[100] = {};
    ssize_t n = read(fd, rbuf, sizeof(rbuf));
    if (n < 0) {
        die("read");
    }
    
    // 5. Print result and hang up
    printf("server says: %s\n", rbuf);
    close(fd);

    return 0;
}