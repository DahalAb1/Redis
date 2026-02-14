/*
************************************************************************************************************************************
review all the code later 
************************************************************************************************************************************
*/


#include <stdint.h>       
#include <stdlib.h>        
#include <string.h>        
#include <stdio.h>         
#include <errno.h>         
#include <unistd.h>        
#include <arpa/inet.h>     
#include <sys/socket.h>    


// A helper function to print errors and exit the program
// (Because you don't want to keep running if you can't open a socket)
static void die(const char *msg) {
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}

static void msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
}

static void do_something(int connfd) {
    char rbuf[64] = {};
    // READ: Wait for the client to send data.
    // NOTE: This assumes we get the whole message in one go. 

    ssize_t n = read(connfd, rbuf, 6);
    if (n < 0) {
        msg("read() error");
        return;
    }
    printf("client says: %s\n", rbuf);

    // WRITE: Send "world" back
    char wbuf[] = "world";
    write(connfd, wbuf, sizeof(wbuf));
}

int main() {

    /*
     1. OBTAIN A FILE DESCRIPTOR (Buy the phone)
         AF_INET = IPv4, SOCK_STREAM = TCP
    
     A socket is defined by two things: 
    domain  → address family (how we address machines)
    type    → communication style (how data is delivered)

    0 means: “Pick the protocol that normally goes with this domain + type.”

    */ 
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        die("socket()");
    }

    // 2. CONFIGURE THE SOCKET (The "Reuse" Hack)
    // This allows you to restart the server immediately without waiting 
    // for the OS to release the port.
    int val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    // 3. BIND (Assign the phone number)
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);     // Port 1234 (converted to network byte order)
    addr.sin_addr.s_addr = htonl(0); // Wildcard IP 0.0.0.0 (listen on all interfaces)

    int rv = bind(fd, (const struct sockaddr *)&addr, sizeof(addr));
    if (rv) {
        die("bind()");
    }

    // 4. LISTEN (Turn the ringer on)
    // SOMAXCONN is the max size of the queue for pending connections
    rv = listen(fd, SOMAXCONN);
    if (rv) {
        die("listen()");
    }

    // 5. ACCEPT LOOP (The Receptionist)
    while (true) {
        // ACCEPT: Wait for a new connection
        struct sockaddr_in client_addr = {};
        socklen_t socklen = sizeof(client_addr);
        
        // This blocks! It stops here until someone connects.
        // It returns a *new* file descriptor (connfd) just for this client.
        int connfd = accept(fd, (struct sockaddr *)&client_addr, &socklen);
        if (connfd < 0) {
            continue;   // Error, just ignore and try again
        }

        // TALK: Read from and write to the client
        do_something(connfd); 

        // HANG UP: We are done with this client.
        // In this simple server, we only handle one message per connection.
        close(connfd);
    }

    return 0;
}

/* There's a problem with this code, it assumes that when we read(), we get entire 
message at once. Because TCP is a stream (not a list of distincy messages), this is technically "broken" code. 
We need to fix this code by introducing a Protocol to define where a message stars and ends*/
