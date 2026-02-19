#include <sys/socket.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <unistd.h>        


int main() { 
    int fd = socket(AF_INET,SOCK_STREAM,0); 
    if(fd < 0){ 
        printf("SOCKET NOT INITIALIZED: ERROR");
    }
    int a = 1; 
    setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&a,sizeof(a));

    struct sockaddr_in addr = {}; 
    addr.sin_port = htons(1234); 
    addr.sin_family = AF_INET; 
    addr.sin_addr.s_addr = htonl(0);

    socklen_t l = sizeof(addr); 
    int rv = bind(fd,(struct sockaddr*)&addr,l);
    if(rv){ 
        printf("ERROR: LINE 25, bind()"); 
    }

    rv = listen(fd, SOMAXCONN);
    if (rv) {
        printf("Cannot make more connections...");
    }

    while(true){ 
        struct sockaddr_in client = {}; 
        socklen_t client_conn_len = sizeof(client); 
        int connfd = accept(fd, (struct sockaddr*)&client,&client_conn_len); 
        if(connfd < 0){ 
            printf("No Connection\n"); 
        }else{ 
            printf("CONNECTION ESTABLISHED\n"); 
        }
        
        close(connfd);
    }
    return 0; 
}