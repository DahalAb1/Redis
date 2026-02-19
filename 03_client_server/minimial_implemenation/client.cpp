#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <stdio.h>

int main() { 
    int fd = socket(AF_INET,SOCK_STREAM ,0);

    struct sockaddr_in addr = {};
    addr.sin_port = htons(1234);
    addr.sin_family = AF_INET; 
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); 

    socklen_t length = sizeof(addr); 
    int est = connect(fd,(const struct sockaddr*)&addr,length);
    if(est == 0){
        printf("CONNECTION ESTABLISHED WITH SERVER\n"); 
    }

    return 0; 
}